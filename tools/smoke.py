#!/usr/bin/env python3
"""DragonBreath attended release smoke test over the network API v2.

Run it after an OTA install, with a person at the heater:

    python3 tools/smoke.py HOST [--token TOKEN] [--steps 1,2,3,4,5]

Steps (see docs/SMOKE_TEST.md):
  1  read-only: state, health, /metrics (if the firmware has it)
  2  heat on/off at a low target, relay on, then OFF within about 1 s
  3  comms watchdog: stop the lease heartbeat and check the latched trip
  4  persistence: reboot, check the fault is still latched, then clear it
  5  panic-off: hold a front-panel button 2 s while it heats

The tool asks before each step that heats or changes a setting. Each heating step
sends OFF in a finally block, also on an error or Ctrl-C. Standard library only.
"""
import argparse
import json
import os
import signal
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

MAX_TARGET_C = 50.0          # hard ceiling of this tool, below the firmware 70 C cap
MIN_TARGET_C = 30.0          # firmware minimum mode target
ACTOR = {"kind": "web", "id": "smoke-test"}
TRIP_REASONS = ("lease expired", "link lost")
REQUIRED_METRICS = (
    "dragonbreath_build_info", "dragonbreath_uptime_seconds",
    "dragonbreath_temperature_celsius", "dragonbreath_sensor_status",
    "dragonbreath_heater_relay_on", "dragonbreath_fault_latched", "dragonbreath_mode",
)
STEP_NAMES = {1: "read-only", 2: "heat on/off", 3: "comms watchdog",
              4: "persistence", 5: "panic-off"}


class Fail(Exception):
    pass


class Skip(Exception):
    pass


class Client:
    def __init__(self, host, token, timeout=4.0):
        self.base = host.rstrip("/") if host.startswith("http") else "http://" + host
        self.token = token
        self.timeout = timeout

    def request(self, method, path, body=None, auth=False, text=False):
        """Return (HTTP status, parsed JSON or text). An HTTP error is not an exception."""
        data = None if body is None else json.dumps(body).encode()
        if method == "POST" and data is None:
            data = b""
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if body is not None:
            req.add_header("Content-Type", "application/json")
        if auth:
            req.add_header("X-DragonBreath-Auth", self.token)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                status, raw = r.status, r.read()
        except urllib.error.HTTPError as e:
            with e:
                status, raw = e.code, e.read()
        if text:
            return status, raw.decode("utf-8", "replace")
        try:
            return status, json.loads(raw or b"{}")
        except ValueError:
            return status, {"error": "not_json", "message": raw[:200].decode("utf-8", "replace")}

    def get(self, path):
        status, data = self.request("GET", path)
        if status != 200:
            raise Fail(f"GET {path}: HTTP {status}")
        return data

    def state(self):
        return self.get("/api/v2/state")

    def command(self, name, revision=None, **args):
        body = {"api_version": 2, "request_id": uuid.uuid4().hex,
                "actor": ACTOR, "command": dict(name=name, **args)}
        if revision is not None:
            body["expected_revision"] = revision
        return self.request("POST", "/api/v2/command", body, auth=True)

    def heartbeat(self, lease_id):
        return self.request("POST", "/api/v2/heartbeat",
                            {"api_version": 2, "lease_id": lease_id}, auth=True)

    def set_settings(self, **values):
        query = urllib.parse.urlencode(values)
        return self.request("POST", "/settings?" + query, auth=True)


class Heartbeat(threading.Thread):
    """Refresh one lease until stop(). Records the send time of the last accepted beat."""

    def __init__(self, client, lease_id, period):
        super().__init__(daemon=True)
        self.client, self.lease_id, self.period = client, lease_id, period
        self.stop_evt = threading.Event()
        self.ok, self.lost, self.last_sent, self.error = 0, 0, None, None

    def run(self):
        while not self.stop_evt.wait(self.period):
            sent = time.monotonic()
            try:
                status, data = self.client.heartbeat(self.lease_id)
            except OSError:               # one lost request is not fatal: retry
                self.lost += 1
                continue
            if status == 200 and data.get("ok"):
                self.ok, self.last_sent = self.ok + 1, sent
            else:
                self.error = f"HTTP {status} {data.get('error')}"
                return

    def stop(self):
        self.stop_evt.set()
        if self.is_alive():
            self.join(10)


def parse_metrics(text):
    """Tiny Prometheus text 0.0.4 parser: {name: [(labels dict, value)]}."""
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        labels = {}
        if "{" in line:
            name, rest = line.split("{", 1)
            body, value = rest.rsplit("}", 1)
            for part in body.split('",'):
                if "=" in part:
                    k, v = part.split("=", 1)
                    labels[k.strip().lstrip(",")] = v.strip().strip('"')
        else:
            name, value = line.split(None, 1)
        out.setdefault(name.strip(), []).append((labels, float(value.split()[0])))
    return out


def fmt_c(sensor):
    t = sensor.get("temperature_c")
    return f"{t:.1f} C ({sensor.get('status')})" if t is not None else f"-- ({sensor.get('status')})"


class Smoke:
    def __init__(self, args, ask=input):
        self.a, self.ask = args, ask
        self.c = Client(args.host, args.token)
        self.trip_latched = False
        self.heated = False   # True after the first power_on: then main() sends a last OFF
        self.results = []

    def confirm(self, text):
        answer = self.ask(f"\n{text}\nContinue? [y/N] ").strip().lower()
        if answer not in ("y", "yes"):
            raise Skip("declined by the operator")

    def poll(self, cond, timeout, period=0.1):
        """Poll /state until cond(state) is true. Return (state, seconds) or (last state, None)."""
        t0 = time.monotonic()
        st = None
        while time.monotonic() - t0 <= timeout:
            try:
                st = self.c.state()
            except (OSError, Fail):
                st = None
            if st is not None and cond(st):
                return st, time.monotonic() - t0
            time.sleep(period)
        return st, None

    def off(self):
        """Send OFF (revision-independent). Retry. Never raises."""
        for _ in range(3):
            try:
                status, data = self.c.command("off")
                if status == 200 and data.get("ok"):
                    return True
            except OSError:
                pass
            time.sleep(0.5)
        print("!!! COULD NOT SEND OFF. Switch the heater off at the mains NOW. !!!")
        return False

    def precheck(self, target):
        st = self.c.state()
        saf = st["safety"]
        if saf.get("fault_latched") or saf.get("inhibited"):
            raise Fail(f"fault latched ({saf.get('reason')}): clear it first (step 4)")
        if st["mode"] != "off":
            raise Fail(f"mode is {st['mode']} (source {st['source']}): stop the other controller first")
        for name, s in st["sensors"].items():
            if s.get("status") != "ok":
                raise Fail(f"{name} sensor status is {s.get('status')}")
        chamber = st["sensors"]["chamber"]["temperature_c"]
        if chamber > target - 2:
            raise Fail(f"chamber is {chamber:.1f} C, above target-2 ({target - 2:.1f} C): let it cool")
        return st

    def power_on(self, target):
        """Send POWER_ON at the current revision, start the heartbeat. Return (lease, hb, t_sent)."""
        st = self.c.state()
        self.heated = True
        t_sent = time.monotonic()
        status, r = self.c.command("power_on", st["state_revision"], target_c=target)
        if status != 200 or not r.get("ok"):
            raise Fail(f"power_on rejected: HTTP {status} {r.get('error')}: {r.get('message')}")
        lease = r.get("lease_id")
        if not isinstance(lease, str) or len(lease) != 32:
            raise Fail("power_on response has no 32-character lease_id")
        if r["state"]["mode"] != "power_on" or not r["state"]["control"]["lease"]["active"]:
            raise Fail("power_on accepted but state is not power_on with an active lease")
        ttl = r["state"]["config"]["comms_ms"] / 1000.0
        hb = Heartbeat(self.c, lease, period=min(2.0, ttl / 4))
        hb.start()
        return lease, hb, t_sent

    def wait_relay_on(self, hb):
        st, dt = self.poll(lambda s: s["heater"]["output"] or s["safety"]["fault_latched"],
                           self.a.heat_timeout, 0.25)
        if dt is None:
            raise Fail(f"heater.output did not go true in {self.a.heat_timeout:.0f} s")
        if st["safety"]["fault_latched"]:
            raise Fail(f"fault latched while heating: {st['safety']['reason']}")
        if hb.error:
            raise Fail(f"heartbeat rejected: {hb.error}")
        print(f"  relay on after {dt:.1f} s (chamber {fmt_c(st['sensors']['chamber'])}, "
              f"PTC {fmt_c(st['sensors']['ptc'])})")
        return st

    # ---- steps ---------------------------------------------------------------

    def step1(self):
        st, health = self.c.state(), self.c.get("/api/v2/health")
        saf = st["safety"]
        print(f"  firmware {st.get('firmware')}  device {st.get('device_id')}  boot {st.get('boot_id')}")
        if "reset_reason" in st:
            print(f"  reset reason {st['reset_reason']}")
        print(f"  uptime {health.get('uptime_ms', 0) / 1000:.0f} s  heap {health.get('free_heap_bytes')} B"
              f"  RSSI {health.get('wifi_rssi_dbm')} dBm")
        print(f"  chamber {fmt_c(st['sensors']['chamber'])}  PTC {fmt_c(st['sensors']['ptc'])}")
        print(f"  mode {st['mode']}  source {st['source']}  comms_ms {st['config']['comms_ms']}"
              f"  fault {saf.get('fault_latched')} ({saf.get('reason')})")
        notes = []
        bad = [n for n, s in st["sensors"].items() if s.get("status") != "ok"]
        if bad:
            raise Fail(f"sensor not ok: {', '.join(bad)}")
        status, text = self.c.request("GET", "/metrics", text=True)
        if status == 404:
            notes.append("/metrics not present (404), skipped")
        elif status != 200:
            raise Fail(f"GET /metrics: HTTP {status}")
        else:
            m = parse_metrics(text)
            missing = [f for f in REQUIRED_METRICS if not m.get(f)]
            if missing:
                raise Fail(f"/metrics misses {', '.join(missing)}")
            latched = m["dragonbreath_fault_latched"][0][1] == 1
            if latched != bool(saf.get("fault_latched") or saf.get("inhibited")):
                raise Fail("dragonbreath_fault_latched does not agree with /api/v2/state")
            notes.append(f"/metrics ok ({len(m)} series names)")
        if saf.get("fault_latched") or saf.get("inhibited"):
            if not self.a.allow_fault:
                raise Fail(f"fault latched: {saf.get('reason')} (use --allow-fault to continue to step 4)")
            notes.append(f"fault latched ({saf.get('reason')}), allowed")
        if st["mode"] != "off":
            notes.append(f"mode is {st['mode']}, heating steps will refuse")
        return "; ".join(notes)

    def step2(self):
        t = self.a.target
        self.precheck(t)
        self.confirm(f"Step 2 HEATS: POWER_ON at {t:.0f} C until the relay is on "
                     f"(max {self.a.heat_timeout:.0f} s), then OFF.")
        hb = None
        try:
            _, hb, _ = self.power_on(t)
            self.wait_relay_on(hb)
            t0 = time.monotonic()
            status, r = self.c.command("off")
            if status != 200 or not r.get("ok"):
                raise Fail(f"OFF rejected: HTTP {status} {r.get('error')}")
            st, _ = self.poll(lambda s: not s["heater"]["output"], self.a.off_deadline, 0.05)
            dt = time.monotonic() - t0
            if st is None or st["heater"]["output"]:
                raise Fail(f"heater.output still true {dt:.2f} s after OFF")
            if dt > self.a.off_deadline:
                raise Fail(f"relay off after {dt:.2f} s, limit {self.a.off_deadline:.2f} s")
            if st["mode"] != "off" or st["control"]["lease"]["active"]:
                raise Fail(f"after OFF: mode {st['mode']}, lease active {st['control']['lease']['active']}")
            return f"relay off {dt:.2f} s after OFF"
        finally:
            if hb:
                hb.stop()
            self.off()

    def step3(self):
        t = self.a.target
        self.precheck(t)
        settings = self.c.get("/settings")
        orig, ttl_ms = settings["comms_ms"], settings["comms_ms_min"]
        ttl = ttl_ms / 1000.0
        self.confirm(f"Step 3 HEATS and CHANGES A SETTING: comms_ms {orig} -> {ttl_ms}, POWER_ON at "
                     f"{t:.0f} C, then stop the heartbeat. The watchdog must latch a fault in about "
                     f"{ttl:.0f} s. The tool restores comms_ms={orig} at the end.")
        hb = None
        try:
            status, r = self.c.set_settings(comms_ms=ttl_ms)
            if status != 200 or r.get("comms_ms") != ttl_ms:
                raise Fail(f"could not set comms_ms={ttl_ms}: HTTP {status} {r}")
            _, hb, t_on = self.power_on(t)
            self.wait_relay_on(hb)
            hb.stop()
            if hb.error:
                raise Fail(f"heartbeat rejected: {hb.error}")
            t_ref = hb.last_sent or t_on
            print(f"  heartbeat stopped after {hb.ok} beats; waiting for the trip")
            st, _ = self.poll(lambda s: s["safety"]["fault_latched"],
                              ttl + self.a.trip_margin + 2 - (time.monotonic() - t_ref), 0.1)
            dt = time.monotonic() - t_ref
            if st is None or not st["safety"]["fault_latched"]:
                raise Fail(f"no fault latched {dt:.1f} s after the last heartbeat")
            self.trip_latched = True
            reason = st["safety"]["reason"] or ""
            if not any(k in reason.lower() for k in TRIP_REASONS):
                raise Fail(f"fault reason is not a comms/lease loss: {reason!r}")
            if st["heater"]["output"] or st["mode"] != "off":
                raise Fail(f"after trip: output {st['heater']['output']}, mode {st['mode']}")
            if not ttl - 0.25 <= dt <= ttl + self.a.trip_margin:
                raise Fail(f"trip after {dt:.1f} s, expected {ttl:.0f} s (+{self.a.trip_margin:.1f} s)")
            return f"tripped after {dt:.1f} s ({reason})"
        finally:
            if hb:
                hb.stop()
            self.off()
            try:
                status, r = self.c.set_settings(comms_ms=orig)
            except OSError:
                status, r = 0, {}
            if status != 200 or r.get("comms_ms") != orig:
                raise Fail(f"COULD NOT RESTORE comms_ms={orig}: HTTP {status}. Set it on /setup.")
            print(f"  comms_ms restored to {orig}")

    def clear_fault(self):
        st = self.c.state()
        status, r = self.c.command("clear_fault", st["state_revision"])
        if status != 200 or not r.get("ok"):
            raise Fail(f"clear_fault rejected: HTTP {status} {r.get('error')}: {r.get('message')}")
        time.sleep(1.5)   # three control ticks: a fault that still holds latches again
        st = self.c.state()
        if st["safety"]["fault_latched"] or st["mode"] != "off":
            raise Fail(f"fault did not clear: {st['safety']['reason']}, mode {st['mode']}")

    def step4(self):
        st = self.c.state()
        if not st["safety"]["fault_latched"]:
            raise Skip("no fault is latched (run step 3 first)")
        boot, reason = st["boot_id"], st["safety"]["reason"]
        answer = self.ask(f"\nStep 4: fault latched ({reason}). Power-cycle the heater now "
                          "(unplug it for 10 s), then press Enter.\nOr type 'r' to restart "
                          "it through the API, 's' to skip: ").strip().lower()
        if answer == "s":
            raise Skip("skipped by the operator")
        if answer == "r":
            status, r = self.c.request("POST", "/api/v2/restart", auth=True)
            if status != 200:
                raise Fail(f"restart refused: HTTP {status} {r.get('error')}")
        print(f"  waiting for a new boot_id (max {self.a.reboot_timeout:.0f} s)")
        st, dt = self.poll(lambda s: s["boot_id"] != boot, self.a.reboot_timeout, 1.0)
        if dt is None:
            raise Fail("boot_id did not change: the heater did not reboot")
        saf = st["safety"]
        print(f"  rebooted after {dt:.0f} s: firmware {st.get('firmware')} "
              f"reset {st.get('reset_reason', '-')}  fault {saf['fault_latched']} ({saf['reason']})")
        if not saf["fault_latched"]:
            raise Fail("fault is NOT latched after the reboot")
        if st["mode"] != "off" or st["heater"]["output"]:
            raise Fail(f"after reboot: mode {st['mode']}, output {st['heater']['output']}")
        self.clear_fault()
        self.trip_latched = False
        return f"latched after reboot ({saf['reason']}), cleared"

    def step5(self):
        t = self.a.target
        self.precheck(t)
        self.confirm(f"Step 5 HEATS: POWER_ON at {t:.0f} C. When the relay is on, HOLD any "
                     "front-panel button for 2 s.")
        hb = None
        try:
            lease, hb, _ = self.power_on(t)
            self.wait_relay_on(hb)
            print(f"  >>> HOLD a front-panel button for 2 s now (max {self.a.panic_timeout:.0f} s) <<<")
            st, dt = self.poll(lambda s: s["mode"] == "off", self.a.panic_timeout, 0.1)
            if dt is None:
                raise Fail("mode did not change to off: no panic-off seen")
            st, _ = self.poll(lambda s: not s["heater"]["output"], self.a.off_deadline, 0.05)
            if st is None or st["heater"]["output"]:
                raise Fail("relay still on after panic-off")
            if st["source"] != "button":
                raise Fail(f"mode off but source is {st['source']}, not button")
            hb.stop()
            status, r = self.c.heartbeat(lease)
            if status != 409 or r.get("error") != "stale_lease":
                raise Fail(f"old lease still accepted after panic-off: HTTP {status}")
            note = f"relay off, mode off, source button ({st['safety']['reason']})"
        finally:
            if hb:
                hb.stop()
            self.off()
        if st["safety"]["fault_latched"]:
            self.clear_fault()   # the panic-off latch is a manual stop, RAM only
            note += ", latch cleared"
        return note

    def run(self, steps):
        results = self.results
        for n in steps:
            print(f"\n== Step {n}: {STEP_NAMES[n]} ==")
            try:
                status, note = "PASS", getattr(self, f"step{n}")() or ""
            except Skip as e:
                status, note = "SKIP", str(e)
            except Fail as e:
                status, note = "FAIL", str(e)
            except (OSError, KeyError, TypeError, ValueError) as e:
                status, note = "FAIL", f"{type(e).__name__}: {e}"
            print(f"[{status}] step {n} {STEP_NAMES[n]}: {note}")
            results.append((n, status, note))
            if n == 1 and status == "FAIL":
                print("Step 1 failed: the other steps are not run.")
                break
        if self.trip_latched and 4 not in steps:
            print("\nThe watchdog fault from step 3 is still latched. Clearing it.")
            try:
                self.clear_fault()
            except (Fail, OSError) as e:
                results.append((3, "FAIL", f"could not clear the step 3 fault: {e}"))
        return results


def parse_args(argv):
    p = argparse.ArgumentParser(description="Attended DragonBreath smoke test over API v2.")
    p.add_argument("host", help="device host or IP (or http://host:port)")
    p.add_argument("--token", default=os.environ.get("DRAGONBREATH_TOKEN", "smoke"),
                   help="control token (X-DragonBreath-Auth); any value if none is set")
    p.add_argument("--steps", default="1,2,3,4,5", help="comma list of steps, default all")
    p.add_argument("--target", type=float, default=40.0, help="heat target in C (30-50, default 40)")
    p.add_argument("--allow-fault", action="store_true",
                   help="step 1 passes with a latched fault (to run step 4 on an earlier trip)")
    p.add_argument("--heat-timeout", type=float, default=30.0, help="seconds to wait for relay on")
    p.add_argument("--off-deadline", type=float, default=1.0, help="seconds from OFF to relay off")
    p.add_argument("--trip-margin", type=float, default=2.0, help="seconds allowed after the timeout")
    p.add_argument("--reboot-timeout", type=float, default=180.0)
    p.add_argument("--panic-timeout", type=float, default=60.0)
    a = p.parse_args(argv)
    if not MIN_TARGET_C <= a.target <= MAX_TARGET_C:
        p.error(f"--target must be {MIN_TARGET_C:.0f}-{MAX_TARGET_C:.0f} C")
    try:
        a.steps = sorted({int(s) for s in a.steps.split(",") if s.strip()})
    except ValueError:
        p.error("--steps takes numbers, for example 1,2")
    if not a.steps or any(s not in STEP_NAMES for s in a.steps):
        p.error("--steps takes numbers 1-5")
    return a


def _sigterm(*_):
    raise KeyboardInterrupt


def main(argv=None, ask=input):
    args = parse_args(argv)
    if threading.current_thread() is threading.main_thread():
        signal.signal(signal.SIGTERM, _sigterm)
    smoke = Smoke(args, ask)
    print(f"DragonBreath smoke test: {smoke.c.base}, steps {args.steps}, target {args.target:.0f} C")
    results = smoke.results
    try:
        smoke.run(args.steps)
    except KeyboardInterrupt:
        print("\nInterrupted. Sending OFF.")
        results.append((0, "FAIL", "interrupted"))
    finally:
        if smoke.heated:
            smoke.off()
    print("\n== Summary ==")
    for n, status, note in results:
        print(f"  {status:4}  {n} {STEP_NAMES.get(n, 'abort')}: {note}")
    failed = any(status == "FAIL" for _, status, _ in results)
    print("RESULT:", "FAIL" if failed else "PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
