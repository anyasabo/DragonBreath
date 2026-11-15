#!/usr/bin/env python3
"""DragonBreath attended release smoke test over the network API v2.

Run it after an OTA install, with a person at the heater:

    python3 tools/smoke.py HOST [--token TOKEN] [--steps 1,2,3,4,5]

Steps (see docs/SMOKE_TEST.md):
  1  read-only: token, state, health, /metrics (if the firmware has it)
  2  heat on/off at a low target, relay on, then OFF within about 1 s
  3  comms watchdog: stop the lease heartbeat and check the latched trip
  4  persistence: reboot, check the fault is still latched, then clear it
  5  panic-off: hold a front-panel button 2 s while it heats

The tool asks before each step that heats or changes a setting. While a POWER_ON
can be active, the tool is "armed": each exit path sends OFF and checks that the
relay is off and the mode is off. Standard library only.
"""
import argparse
import contextlib
import http.client
import json
import os
import signal
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

MAX_TARGET_C = 50.0          # hard ceiling of this tool, below the firmware 70 C cap
MIN_TARGET_C = 30.0          # firmware minimum mode target
POLL_S = 0.25
ACTOR = {"kind": "web", "id": "smoke-test"}
TRIP_REASONS = ("lease expired", "link lost")
RESTORED_REASON = "safety trip"   # reason of a persisted lease/link trip after a reboot
REQUIRED_METRICS = (
    "dragonbreath_build_info", "dragonbreath_uptime_seconds",
    "dragonbreath_temperature_celsius", "dragonbreath_sensor_status",
    "dragonbreath_heater_relay_on", "dragonbreath_fault_latched", "dragonbreath_mode",
)
STEP_NAMES = {1: "read-only", 2: "heat on/off", 3: "comms watchdog",
              4: "persistence", 5: "panic-off"}
ROLLBACK = "Do not keep this firmware: roll back with /fw or Boot inactive slot (docs/SMOKE_TEST.md)."
STEP_ACTIONS = {
    1: "Fix the cause above before a heat test. If a sensor reads wrong, do not keep this firmware.",
    2: ROLLBACK, 3: ROLLBACK + " Check comms_ms in the dashboard Safety settings.",
    4: ROLLBACK, 5: ROLLBACK,
}
ERRORS = {
    "auth_failed": "wrong --token (the device rejected X-DragonBreath-Auth)",
    "revision_conflict": "the device state changed (another controller or a button); nothing was done",
    "fault_latched": "a fault is latched; clear it first",
    "inhibited": "heat is inhibited until a reboot",
    "stale_lease": "the lease is not valid any more",
    "persist_failed": "the device could not save to NVS; the fault stays latched",
    "heater_active": "the heater is on, so the device refuses",
    "invalid_command": "the device rejected the request as invalid",
}


class Fail(Exception):
    def __init__(self, msg, action=None):
        super().__init__(msg)
        self.action = action


class Skip(Exception):
    pass


class NetError(OSError):
    """No usable answer from the device (refused, timeout, broken response)."""


def explain(what, status, data):
    code = data.get("error")
    return f"{what} refused: {ERRORS.get(code, code or f'HTTP {status}')}"


class Client:
    def __init__(self, host, token, timeout=4.0):
        self.base = host.rstrip("/") if host.startswith("http") else "http://" + host
        self.token = token
        self.timeout = timeout

    def request(self, method, path, body=None, auth=False, text=False):
        """Return (HTTP status, JSON or text). Raise NetError if no complete answer arrives."""
        data = None if body is None else json.dumps(body).encode()
        if method == "POST" and data is None:
            data = b""
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if body is not None:
            req.add_header("Content-Type", "application/json")
        if auth:
            req.add_header("X-DragonBreath-Auth", self.token)
        try:
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as r:
                    status, raw = r.status, r.read()
            except urllib.error.HTTPError as e:
                with e:
                    status, raw = e.code, e.read()
        except (OSError, http.client.HTTPException) as e:
            raise NetError(f"{method} {path}: {e}") from e
        if status == 403 and auth:
            raise Fail(f"{method} {path}: {ERRORS['auth_failed']}", "Run again with the correct --token.")
        if text:
            return status, raw.decode("utf-8", "replace")
        try:
            return status, json.loads(raw or b"{}")
        except ValueError:
            return status, {"message": raw[:200].decode("utf-8", "replace")}

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
        return self.request("POST", "/settings?" + urllib.parse.urlencode(values), auth=True)


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


def is_off(st):
    return not st["heater"]["output"] and st["mode"] == "off"


class Smoke:
    def __init__(self, args, ask=input):
        self.a, self._ask = args, ask
        self.c = Client(args.host, args.token)
        self.armed = False          # a POWER_ON can be active: every exit path sends OFF
        self.alarmed = False        # the mains warning was given
        self.lease, self.next_beat, self.beat_period = None, 0.0, 2.0
        self.beats, self.last_beat = 0, None
        self.trip_latched = False   # this run latched a lease/link trip (step 3)
        self.results = []           # (label, PASS/FAIL/SKIP, note, action)

    def add(self, label, status, note, action=None):
        self.results.append((label, status, note, action))

    def ask(self, prompt):
        try:
            return self._ask(prompt).strip().lower()
        except EOFError:
            raise KeyboardInterrupt from None

    def confirm(self, text):
        if self.ask(f"\n{text}\nContinue? [y/N] ") not in ("y", "yes"):
            raise Skip("declined by the operator")

    # ---- lease heartbeat, sent from the poll loop of the main thread ----------

    def beat(self):
        """Refresh the lease when it is due. Never raises. A stuck main loop lets the lease expire."""
        if not self.lease or time.monotonic() < self.next_beat:
            return
        sent = time.monotonic()
        self.next_beat = sent + self.beat_period
        try:
            status, data = self.c.heartbeat(self.lease)
        except (OSError, Fail):
            return                  # one lost heartbeat is not fatal
        if status == 200 and data.get("ok"):
            self.beats, self.last_beat = self.beats + 1, sent
        else:
            print(f"  heartbeat {explain('heartbeat', status, data)}")
            self.lease = None

    def poll(self, cond, timeout, period=POLL_S):
        """Poll /state (and heartbeat) until cond(state). Return (state, seconds) or (last state, None)."""
        t0 = time.monotonic()
        st = None
        while True:
            self.beat()
            try:
                st = self.c.state()
            except (OSError, Fail):
                pass
            else:
                if cond(st):
                    return st, time.monotonic() - t0
            if time.monotonic() - t0 > timeout:
                return st, None
            time.sleep(period)

    # ---- OFF ----------------------------------------------------------------

    def off_and_verify(self, window):
        """Send OFF, then poll for relay off and mode off. Never raises. Return (state, seconds or None)."""
        t0 = time.monotonic()
        self.lease = None
        for _ in range(3):
            try:
                status, data = self.c.command("off")
                if status == 200 and data.get("ok"):
                    break
            except (OSError, Fail):
                pass
            time.sleep(POLL_S)
        st, dt = self.poll(is_off, window)
        if dt is None:
            return st, None
        self.armed = False
        return st, time.monotonic() - t0

    def safe_off(self):
        """Used on each exit path while armed. Never raises."""
        if not self.armed or self.alarmed:
            return
        st, dt = self.off_and_verify(self.a.off_verify)
        if dt is None:
            self.alarmed = True
            seen = (f"relay on={st['heater']['output']}, mode={st['mode']}" if st
                    else "the device does not answer")
            print("\n!!! The heater did not confirm OFF. Switch the heater off at the mains NOW. !!!")
            self.add("OFF check", "FAIL", f"OFF not confirmed in {self.a.off_verify:.0f} s ({seen})",
                     "Switch the heater off at the mains NOW. " + ROLLBACK)

    # ---- heating session shared by steps 2, 3 and 5 ---------------------------

    def precheck(self):
        t = self.a.target
        settings = self.c.get("/settings")
        if t > settings.get("max", MAX_TARGET_C):
            raise Fail(f"--target {t:.0f} C is above the device max {settings['max']:.0f} C")
        st = self.c.state()
        saf = st["safety"]
        if saf.get("fault_latched") or saf.get("inhibited"):
            raise Fail(f"a fault is latched ({saf.get('reason')})", "Clear it first (step 4 or the dashboard).")
        if st["mode"] != "off":
            raise Fail(f"mode is {st['mode']} (source {st['source']})", "Stop the other controller first.")
        for name, s in st["sensors"].items():
            if s.get("status") != "ok":
                raise Fail(f"{name} sensor status is {s.get('status')}")
        chamber = st["sensors"]["chamber"]["temperature_c"]
        if chamber > t - 2:
            raise Fail(f"chamber is {chamber:.1f} C, above target-2 ({t - 2:.1f} C)",
                       "Let the chamber cool, or use a higher --target (max 50).")
        return st

    @contextlib.contextmanager
    def heating(self, text, setup=None):
        """precheck -> confirm -> [setup] -> POWER_ON at the precheck revision -> body -> OFF."""
        st = self.precheck()
        self.confirm(text)
        cleanup = None
        try:
            if setup:
                cleanup = setup()
            self.armed = True       # from here, an unknown result can mean heat
            t_sent = time.monotonic()
            try:
                status, r = self.c.command("power_on", st["state_revision"], target_c=self.a.target)
            except NetError as e:
                raise Fail(f"no answer to power_on ({e})") from e
            except Fail:
                self.armed = False  # auth rejected: definitely no heat
                raise
            if status != 200 or not r.get("ok"):
                self.armed = False  # definite rejection: no heat, no OFF needed
                raise Fail(explain("power_on", status, r), "Run the step again.")
            lease = r.get("lease_id")
            if not isinstance(lease, str) or len(lease) != 32:
                raise Fail("power_on response has no 32-character lease_id")
            ttl = r["state"]["config"]["comms_ms"] / 1000.0
            self.lease, self.beats, self.last_beat = lease, 0, None
            self.beat_period = min(2.0, ttl / 4)
            self.next_beat = t_sent + self.beat_period
            yield {"t_sent": t_sent, "ttl": ttl, "lease": lease}
        finally:
            self.lease = None
            self.safe_off()
            if cleanup:
                cleanup()

    def wait_relay_on(self):
        st, dt = self.poll(lambda s: s["heater"]["output"] or s["safety"]["fault_latched"]
                           or s["mode"] != "power_on", self.a.heat_timeout)
        if dt is None:
            raise Fail(f"the relay did not switch on in {self.a.heat_timeout:.0f} s")
        if not st["heater"]["output"]:
            raise Fail(f"heat stopped before the relay was on: mode {st['mode']}, "
                       f"fault {st['safety']['reason']}")
        print(f"  relay on after {dt:.1f} s (chamber {fmt_c(st['sensors']['chamber'])}, "
              f"PTC {fmt_c(st['sensors']['ptc'])})")
        return st

    # ---- fault clear --------------------------------------------------------

    def clear_fault(self):
        for attempt in range(2):
            st = self.c.state()
            status, r = self.c.command("clear_fault", st["state_revision"])
            if status == 200 and r.get("ok"):
                break
            if r.get("error") != "revision_conflict" or attempt:
                raise Fail(explain("clear_fault", status, r), "Clear the fault on the dashboard.")
        time.sleep(1.5)   # three control ticks: a fault that still holds latches again
        st = self.c.state()
        if st["safety"]["fault_latched"] or st["mode"] != "off":
            raise Fail(f"the fault did not clear: {st['safety']['reason']}, mode {st['mode']}")
        self.trip_latched = False

    # ---- steps ---------------------------------------------------------------

    def step1(self):
        status, _ = self.c.request("GET", "/api/v2/console", auth=True, text=True)
        if status != 200:
            raise Fail(f"GET /api/v2/console: HTTP {status}")
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
        notes = ["token ok"]
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
                raise Fail(f"a fault is latched: {saf.get('reason')}",
                           "Find the cause. To test its persistence, run --steps 1,4 --allow-fault.")
            notes.append(f"fault latched ({saf.get('reason')}), allowed")
        if st["mode"] != "off":
            notes.append(f"mode is {st['mode']}, heating steps will refuse")
        return "; ".join(notes)

    def step2(self):
        with self.heating(f"Step 2 HEATS: POWER_ON at {self.a.target:.0f} C until the relay is on "
                          f"(max {self.a.heat_timeout:.0f} s), then OFF."):
            self.wait_relay_on()
            _, dt = self.off_and_verify(self.a.off_deadline)
            if dt is None:
                raise Fail(f"relay or mode not off {self.a.off_deadline:.1f} s after OFF")
            return f"relay off {dt:.2f} s after OFF"

    def restore_comms(self, value):
        for _ in range(3):
            try:
                status, r = self.c.set_settings(comms_ms=value)
                if status == 200 and r.get("comms_ms") == value:
                    print(f"  comms_ms restored to {value}")
                    return
            except (OSError, Fail):
                pass
            time.sleep(0.5)
        self.add("comms_ms restore", "FAIL", f"could not set comms_ms back to {value}",
                 f"Set comms_ms to {value} in the dashboard Safety settings.")

    def step3(self):
        settings = self.c.get("/settings")
        orig, ttl_ms = settings["comms_ms"], settings["comms_ms_min"]

        def setup():
            status, r = self.c.set_settings(comms_ms=ttl_ms)
            if status != 200 or r.get("comms_ms") != ttl_ms:
                raise Fail(f"could not set comms_ms={ttl_ms}: HTTP {status}")
            return lambda: self.restore_comms(orig)

        with self.heating(f"Step 3 HEATS and CHANGES A SETTING: comms_ms {orig} -> {ttl_ms}, POWER_ON "
                          f"at {self.a.target:.0f} C, then stop the heartbeat. The watchdog must latch "
                          f"a fault in about {ttl_ms / 1000:.0f} s. The tool restores comms_ms={orig}.",
                          setup) as on:
            ttl = on["ttl"]
            self.wait_relay_on()
            self.poll(lambda s: self.beats >= 1, 3 * self.beat_period)
            if not self.beats:
                raise Fail("no heartbeat was accepted")
            t_ref, self.lease = self.last_beat, None
            print(f"  heartbeat stopped after {self.beats} beats; waiting for the trip")
            st, _ = self.poll(lambda s: s["safety"]["fault_latched"],
                              ttl + self.a.trip_margin + 2 - (time.monotonic() - t_ref))
            dt = time.monotonic() - t_ref
            if st is None or not st["safety"]["fault_latched"]:
                raise Fail(f"no fault latched {dt:.1f} s after the last heartbeat")
            reason = st["safety"]["reason"] or ""
            if not any(k in reason.lower() for k in TRIP_REASONS):
                raise Fail(f"the fault reason is not a comms or lease loss: {reason!r}")
            self.trip_latched = True
            if st["heater"]["output"] or st["mode"] != "off":
                raise Fail(f"after the trip: relay on={st['heater']['output']}, mode {st['mode']}")
            if not ttl - 0.25 <= dt <= ttl + self.a.trip_margin:
                raise Fail(f"trip after {dt:.1f} s, expected {ttl:.0f} s (+{self.a.trip_margin:.1f} s)")
            return f"tripped after {dt:.1f} s ({reason})"

    def step4(self):
        st = self.c.state()
        if not st["safety"]["fault_latched"]:
            raise Skip("no fault is latched (run step 3 first)")
        boot, reason = st["boot_id"], st["safety"]["reason"]
        answer = self.ask(f"\nStep 4: fault latched ({reason}). Power-cycle the heater now "
                          "(unplug it for 10 s), then press Enter.\nOr type 'r' to restart "
                          "it through the API, 's' to skip: ")
        if answer == "s":
            raise Skip("skipped by the operator")
        if answer == "r":
            status, r = self.c.request("POST", "/api/v2/restart", auth=True)
            if status != 200:
                raise Fail(explain("restart", status, r))
        print(f"  waiting for a new boot_id (max {self.a.reboot_timeout:.0f} s)")
        st, dt = self.poll(lambda s: s["boot_id"] != boot, self.a.reboot_timeout, 1.0)
        if dt is None:
            raise Fail("boot_id did not change: the heater did not reboot")
        saf = st["safety"]
        after = saf.get("reason") or ""
        print(f"  rebooted after {dt:.0f} s: firmware {st.get('firmware')} "
              f"reset {st.get('reset_reason', '-')}  fault {saf['fault_latched']} ({after})")
        if not saf["fault_latched"]:
            raise Fail("the fault is NOT latched after the reboot")
        if st["mode"] != "off" or st["heater"]["output"]:
            raise Fail(f"after the reboot: mode {st['mode']}, relay on={st['heater']['output']}")
        ours = self.trip_latched and (after == RESTORED_REASON
                                      or any(k in after.lower() for k in TRIP_REASONS))
        if not ours and self.ask(f"\nThis run did not cause the fault ({after}). "
                                 "Clear it? [y/N] ") not in ("y", "yes"):
            return f"latched after the reboot ({after}), left latched"
        self.clear_fault()
        return f"latched after the reboot ({after}), cleared"

    def step5(self):
        with self.heating(f"Step 5 HEATS: POWER_ON at {self.a.target:.0f} C. When the relay is on, "
                          "HOLD any front-panel button for 2 s.") as on:
            self.wait_relay_on()
            print(f"  >>> HOLD a front-panel button for 2 s now (max {self.a.panic_timeout:.0f} s) <<<")
            st, dt = self.poll(lambda s: s["mode"] == "off", self.a.panic_timeout)
            if dt is None:
                raise Fail("the mode did not change to off: no panic-off seen")
            st, _ = self.poll(lambda s: not s["heater"]["output"], self.a.off_deadline)
            if st is None or st["heater"]["output"]:
                raise Fail("the relay is still on after panic-off")
            if st["source"] != "button":
                raise Fail(f"mode off, but the source is {st['source']}, not button")
            self.lease = None
            status, r = self.c.heartbeat(on["lease"])
            if status != 409 or r.get("error") != "stale_lease":
                raise Fail(f"the old lease is still accepted after panic-off: HTTP {status}")
        reason = st["safety"]["reason"] or ""
        note = f"relay off, mode off, source button ({reason})"
        if st["safety"]["fault_latched"] and "panic" in reason:
            self.clear_fault()   # this run caused the panic-off latch (RAM only)
            note += ", latch cleared"
        return note

    def run(self, steps):
        for n in steps:
            print(f"\n== Step {n}: {STEP_NAMES[n]} ==")
            action = None
            try:
                status, note = "PASS", getattr(self, f"step{n}")() or ""
            except Skip as e:
                status, note = "SKIP", str(e)
            except Fail as e:
                status, note, action = "FAIL", str(e), e.action or STEP_ACTIONS[n]
            except (OSError, KeyError, TypeError, ValueError) as e:
                status, note, action = "FAIL", f"{type(e).__name__}: {e}", STEP_ACTIONS[n]
            print(f"[{status}] step {n} {STEP_NAMES[n]}: {note}")
            self.add(f"{n} {STEP_NAMES[n]}", status, note, action)
            if n == 1 and status == "FAIL":
                print("Step 1 failed: the other steps are not run.")
                break
        if self.trip_latched and 4 not in steps:
            print("\nThe watchdog fault from step 3 is still latched. Clearing it.")
            try:
                self.clear_fault()
            except (Fail, OSError) as e:
                self.add("fault clear", "FAIL", f"could not clear the step 3 fault: {e}",
                         "Clear the fault on the dashboard.")


def parse_args(argv):
    p = argparse.ArgumentParser(description="Attended DragonBreath smoke test over API v2.")
    p.add_argument("host", help="device host or IP (or http://host:port)")
    p.add_argument("--token", default=os.environ.get("DRAGONBREATH_TOKEN", "smoke"),
                   help="control token (X-DragonBreath-Auth); any value if none is set")
    p.add_argument("--steps", default="1,2,3,4,5", help="comma list of steps, default all")
    p.add_argument("--target", type=float, default=40.0, help="heat target in C (30-50, default 40)")
    p.add_argument("--allow-fault", action="store_true",
                   help="step 1 passes with a latched fault, to test its persistence with step 4")
    p.add_argument("--heat-timeout", type=float, default=30.0, help="seconds to wait for relay on")
    p.add_argument("--off-deadline", type=float, default=1.0, help="seconds from OFF to relay off")
    p.add_argument("--off-verify", type=float, default=3.0, help="seconds to confirm a cleanup OFF")
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


def _interrupt(*_):
    raise KeyboardInterrupt


def print_final_state(smoke):
    print("\n== Final device state ==")
    try:
        st = smoke.c.state()
    except (OSError, Fail) as e:
        print(f"  could not read the state ({e}). Look at the heater LEDs.")
        return
    saf = st["safety"]
    print(f"  mode {st['mode']}  relay on={st['heater']['output']}  "
          f"chamber {fmt_c(st['sensors']['chamber'])}  PTC {fmt_c(st['sensors']['ptc'])}")
    if saf.get("fault_latched") or saf.get("inhibited"):
        print(f"  a fault stays latched: {saf.get('reason')}. Clear it on the dashboard when the cause is known.")
    else:
        print("  no fault latched")


def main(argv=None, ask=input):
    args = parse_args(argv)
    sigs = [getattr(signal, n) for n in ("SIGTERM", "SIGHUP") if hasattr(signal, n)]
    old = {}
    for s in sigs:
        try:
            old[s] = signal.signal(s, _interrupt)
        except ValueError:      # not the main thread
            pass
    smoke = Smoke(args, ask)
    print(f"DragonBreath smoke test: {smoke.c.base}, steps {args.steps}, target {args.target:.0f} C")
    try:
        smoke.run(args.steps)
    except KeyboardInterrupt:
        print("\nInterrupted. The run stops.")
        smoke.add("run", "FAIL", "interrupted", "Run the test again.")
    finally:
        try:
            smoke.safe_off()
        except KeyboardInterrupt:
            print("!!! OFF was interrupted. Switch the heater off at the mains NOW. !!!")
        for s, h in old.items():
            signal.signal(s, h)
    print_final_state(smoke)
    print("\n== Summary ==")
    for label, status, note, action in smoke.results:
        print(f"  {status:4}  {label}: {note}")
        if status == "FAIL" and action:
            print(f"        -> {action}")
    failed = any(r[1] == "FAIL" for r in smoke.results)
    print("RESULT:", "FAIL" if failed else "PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
