#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""DragonBreath capture tool: record a named scenario into one capture directory.

    python3 tools/capture.py HOST SCENARIO [options]

Scenarios (each has a guided prompt and an automatic stop):
  idle        no commands; record N minutes (default 30) at 2 Hz
  first-heat  60 s baseline from cold, then the first relay ON, then 2 min of heat,
              then OFF and 60 s of tail
  step        heat to a target (default 50 C) and hold, for the PID fit. Default
              maximum 2.5 h (about 3 time constants of the enclosure)
  cooldown    from heater OFF until the fan stops and PTC and chamber agree
  soak        hours of low-rate samples (default 4 h, state every 10 s)

READ-ONLY BY DEFAULT. The tool reads GET /api/v2/info, /api/v2/state, /settings,
/api/v2/calibration, /api/v2/heater_method, /api/v2/health and /metrics. In a scenario
that needs heat, it tells you what to press on the heater or the dashboard.

With --allow-control and --token-file FILE (or --token TOKEN) the tool sends only
documented API v2 commands: POST /api/v2/command (power_on at the scenario target, off)
and POST /api/v2/heartbeat for its own lease. An accepted power_on also stores its
target as params.manual_target_c on the device (docs/api-v2.md); the front-panel On
button then uses that target. The tool never changes a setting, a calibration or a
safety limit, and never clears a fault. It sends OFF only to its own heat. While its
POWER_ON can be active, each exit path (end, error, Ctrl-C, SIGTERM, SIGHUP) sends OFF
and checks that the relay is off. Signals are ignored while that OFF runs.
Before heat, the tool compares --mains-v with --rated-v and refuses a mismatch.

Output: CAPTURES_DIR/YYYYmmddTHHMMSSZ-SCENARIO/ (see tools/dbcapture_common.py).
Then run: python3 tools/analyze_capture.py THAT_DIR
Standard library only.
"""
import argparse
import datetime
import json
import os
import signal
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dbcapture_common as common  # noqa: E402

TOOL_VERSION = "1"
ACTOR = {"kind": "web", "id": "capture-tool"}
MIN_TARGET_C, MAX_TARGET_C = 30.0, 60.0     # tool ceiling, below the 70 C firmware cap
MAINS_TOLERANCE = 0.15                      # mains vs rated voltage
STOP_SIGNALS = tuple(s for s in (getattr(signal, n, None) for n in ("SIGINT", "SIGTERM", "SIGHUP"))
                     if s is not None)
SCENARIOS = {
    #              rate Hz, metrics s, duration/max s
    "idle":       (2.0, 15.0, 1800.0),
    "first-heat": (2.0, 15.0, 900.0),
    "step":       (1.0, 15.0, 9000.0),
    "cooldown":   (1.0, 15.0, 3600.0),
    "soak":       (0.1, 60.0, 4 * 3600.0),
}
PROMPTS = {
    "idle": "Leave the heater idle: mode off, no heat, fan off. Do not touch it during the capture.",
    "first-heat": ("The heater must be cold: no heat for at least 30 min. The tool records a "
                   "baseline first."),
    "step": ("The chamber must be near room temperature and the printer door closed. "
             "The capture holds the target and stops by itself."),
    "cooldown": "The capture records the cooldown after heat. Do not open the door.",
    "soak": "Long capture at a low rate. Leave the heater in its normal use.",
}


DONE_REASONS = ("duration reached", "first heat recorded", "hold time reached",
                "the mode changed to off", "cool and fan off", "maximum duration reached",
                "maximum duration reached (target not reached)")


class Abort(Exception):
    pass


class Client:
    def __init__(self, host, token=None, timeout=4.0):
        self.base = host.rstrip("/") if host.startswith("http") else "http://" + host
        self.token, self.timeout = token, timeout

    def request(self, method, path, body=None, auth=False):
        """Return (status, bytes). Raise OSError if no answer arrives."""
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
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            with e:
                return e.code, e.read()
        except Exception as e:  # noqa: BLE001 - every transport error is "no answer"
            raise OSError(f"{method} {path}: {e}") from e

    def get_json(self, path):
        status, raw = self.request("GET", path)
        if status != 200:
            return None
        try:
            return json.loads(raw)
        except ValueError:
            return None

    def get_text(self, path):
        status, raw = self.request("GET", path)
        return status, raw.decode("utf-8", "replace")

    def command(self, name, revision=None, **args):
        body = {"api_version": 2, "request_id": uuid.uuid4().hex, "actor": ACTOR,
                "command": dict(name=name, **args)}
        if revision is not None:
            body["expected_revision"] = revision
        status, raw = self.request("POST", "/api/v2/command", body, auth=True)
        try:
            return status, json.loads(raw or b"{}")
        except ValueError:
            return status, {}

    def heartbeat(self, lease):
        status, raw = self.request("POST", "/api/v2/heartbeat",
                                   {"api_version": 2, "lease_id": lease}, auth=True)
        try:
            return status, json.loads(raw or b"{}")
        except ValueError:
            return status, {}


def _g(st, *path):
    for p in path:
        if not isinstance(st, dict):
            return None
        st = st.get(p)
    return st


class Capturer:
    def __init__(self, args, ask=input, say=print):
        self.a, self._ask, self.say = args, ask, say
        self.c = Client(args.host, args.token)
        self.w = None
        self.t0 = None
        self.last = None             # last good state
        self.next_sample = self.next_metrics = 0.0
        self.metrics_ok = True
        self.armed = False           # our POWER_ON can be active
        self.lease = None
        self.beat_period = 2.0
        self.lease_lock = threading.Lock()
        self.beat_stop = threading.Event()
        self.beat_thread = None
        self.loop_alive = 0.0        # time.monotonic() of the last main-loop pass
        self.boot_id = None
        self.stop_reason = None
        self.hot_since = None

    # ---- time and output ----------------------------------------------------

    def now(self):
        return time.monotonic() - self.t0

    def event(self, kind, text):
        if self.w:
            self.w.event(self.now(), kind, text)
        if kind in ("prompt", "warn", "command", "stop", "error"):
            self.say(f"[{self.now():7.1f} s] {text}")

    def ask(self, text):
        if self.a.yes:
            return "y"
        try:
            return self._ask(f"\n{text}\nContinue? [y/N] ").strip().lower()
        except EOFError:
            raise KeyboardInterrupt from None

    # ---- start --------------------------------------------------------------

    def open(self):
        info = self.c.get_json("/api/v2/info")
        if info is None:
            raise Abort(f"no answer from {self.c.base}/api/v2/info. Check the host and the network.")
        hdr = {
            "tool": "tools/capture.py", "tool_version": TOOL_VERSION,
            "scenario": self.a.scenario, "notes": self.a.notes, "host": self.a.host,
            "base_url": self.c.base,
            "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "host_time_unix": time.time(), "host_tz": time.strftime("%z"),
            "ambient_c": self.a.ambient_c, "mains_v": self.a.mains_v, "rated_v": self.a.rated_v,
            "allow_control": bool(self.a.allow_control),
            "params": {k: v for k, v in vars(self.a).items() if k != "token"},
            "info": info, "rref_kohm": info.get("rref_kohm"),
        }
        for key, path in (("settings", "/settings"), ("calibration", "/api/v2/calibration"),
                          ("health", "/api/v2/health")):
            try:
                hdr[key] = self.c.get_json(path)
            except OSError:
                hdr[key] = None
        try:
            hm = self.c.get_json("/api/v2/heater_method")
            hdr["heater_method"] = (hm or {}).get("method")
        except OSError:
            hdr["heater_method"] = None
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        out = os.path.join(self.a.out, f"{stamp}-{self.a.scenario}")
        self.w = common.CaptureWriter(out, hdr)
        self.t0 = time.monotonic()
        self.say(f"capture: {out}")
        self.say(f"device {info.get('device_id')}, firmware {info.get('firmware')}, "
                 f"Rref {info.get('rref_kohm')} k, method {hdr['heater_method']}")
        return out

    # ---- sampling -----------------------------------------------------------

    def sample(self):
        """Take a state sample and a metrics snapshot when due. Return the state or None."""
        t = self.now()
        st = None
        if t >= self.next_sample:
            self.next_sample = t + 1.0 / self.a.rate
            try:
                st = self.c.get_json("/api/v2/state")
            except OSError as e:
                self.event("error", f"state: {e}")
            if isinstance(st, dict) and isinstance(st.get("sensors"), dict):
                bid = st.get("boot_id")
                if self.boot_id and bid and bid != self.boot_id:
                    self.event("warn", f"the device rebooted (boot_id {self.boot_id} -> {bid})")
                self.boot_id = bid or self.boot_id
                self.w.state(common.flatten_state(st, t, time.time()), st)
                self.last = st
            elif st is not None:
                self.event("error", "state: the answer is not a state")
                st = None
        if self.metrics_ok and t >= self.next_metrics:
            self.next_metrics = t + self.a.metrics_every
            try:
                status, text = self.c.get_text("/metrics")
                if status == 200:
                    self.w.metrics(t, text)
                elif status == 404:
                    self.metrics_ok = False
                    self.event("note", "no /metrics on this firmware")
                else:
                    self.event("error", f"metrics: HTTP {status}")
            except OSError as e:
                self.event("error", f"metrics: {e}")
        return st

    def _beat_loop(self, client):
        """Heartbeat thread. The state and /metrics GETs cannot delay it. If the main loop
        stops for more than two periods, the thread stops too: then the lease expires and
        the firmware comms watchdog stops the heat."""
        while not self.beat_stop.wait(self.beat_period):
            with self.lease_lock:
                lease = self.lease
            if not lease:
                return
            if time.monotonic() - self.loop_alive > 2 * self.beat_period + 1.0:
                continue
            try:
                status, data = client.heartbeat(lease)
            except OSError:
                continue
            if status != 200 or not data.get("ok"):
                with self.lease_lock:
                    if self.lease == lease:
                        self.lease = None
                self.event("warn", f"heartbeat refused ({data.get('error') or status}): the lease ended")
                return

    def start_beat(self, lease, ttl_s):
        self.beat_period = max(0.5, min(2.0, ttl_s / 4))
        with self.lease_lock:
            self.lease = lease
        self.loop_alive = time.monotonic()
        self.beat_stop.clear()
        client = Client(self.a.host, self.a.token, timeout=max(0.5, min(2.0, ttl_s / 5)))
        self.beat_thread = threading.Thread(target=self._beat_loop, args=(client,),
                                            name="heartbeat", daemon=True)
        self.beat_thread.start()

    def stop_beat(self):
        with self.lease_lock:
            self.lease = None
        self.beat_stop.set()

    def guard(self, st):
        """Tool-side guard while armed. It can only stop heat."""
        if not self.armed or not st:
            return None
        if _g(st, "safety", "fault_latched"):
            return f"a fault latched: {_g(st, 'safety', 'reason')}"
        owner = _g(st, "control", "lease", "owner")
        if st.get("mode") != "power_on" or (owner and owner != ACTOR["id"]):
            return f"the mode changed (mode {st.get('mode')}, source {st.get('source')})"
        ptc = _g(st, "sensors", "ptc", "temperature_c")
        ch = _g(st, "sensors", "chamber", "temperature_c")
        if ch is not None and ch > self.a.target + 10:
            return f"chamber {ch:.1f} C is more than 10 C above the target"
        if ptc is not None and ptc >= 104.0:
            self.hot_since = self.hot_since if self.hot_since is not None else self.now()
            if self.now() - self.hot_since > 20:
                return f"PTC {ptc:.1f} C for more than 20 s"
        else:
            self.hot_since = None
        return None

    def run_until(self, cond, max_s, label):
        """Sample until cond(state, t) is true or max_s passes. Return the stop reason."""
        start = self.now()
        while True:
            self.loop_alive = time.monotonic()
            st = self.sample()
            if st is not None:
                g = self.guard(st)
                if g:
                    self.disarm_other(st)
                    raise Abort(g)
                if cond and cond(st, self.now() - start):
                    return "condition"
            if self.now() - start >= max_s:
                return "time"
            wait = min(self.next_sample, self.next_metrics if self.metrics_ok else 1e9) - self.now()
            time.sleep(max(0.01, min(wait, 0.5)))

    # ---- control (only with --allow-control) ---------------------------------

    def check_mains(self):
        mv, rv = self.a.mains_v, self.a.rated_v
        if mv is None or rv is None:
            self.event("warn", "no --mains-v and --rated-v: the tool cannot compare the mains with the label")
            return
        if abs(mv - rv) / rv > MAINS_TOLERANCE and not self.a.allow_mains_mismatch:
            raise Abort(f"mains {mv:.0f} V does not agree with the rated {rv:.0f} V. The fan and the PTC "
                        "do not work as designed. Use --allow-mains-mismatch only after a review.")

    def precheck(self):
        self.check_mains()
        st = self.c.get_json("/api/v2/state")
        settings = self.c.get_json("/settings") or {}
        if not st:
            raise Abort("no state from the device")
        if self.a.target > float(settings.get("max", MAX_TARGET_C)):
            raise Abort(f"target {self.a.target:.0f} C is above the device max {settings.get('max')} C")
        if _g(st, "safety", "fault_latched") or _g(st, "safety", "inhibited"):
            raise Abort(f"a fault is latched ({_g(st, 'safety', 'reason')}). The tool never clears "
                        "a fault. Find the cause, then clear it on the dashboard.")
        if st.get("mode") != "off":
            raise Abort(f"mode is {st.get('mode')} (source {st.get('source')}). Stop it first.")
        for name in ("chamber", "ptc"):
            if _g(st, "sensors", name, "status") != "ok":
                raise Abort(f"{name} sensor status is {_g(st, 'sensors', name, 'status')}")
        ch = _g(st, "sensors", "chamber", "temperature_c")
        if ch is not None and ch > self.a.target - 2:
            raise Abort(f"chamber {ch:.1f} C is above target - 2 C. Let it cool or use a higher target.")
        return st

    def power_on(self):
        st = self.precheck()
        if self.ask(f"The tool sends power_on at {self.a.target:.0f} C. Stay at the heater.") not in (
                "y", "yes"):
            raise Abort("declined by the operator")
        self.armed = True
        try:
            status, r = self.c.command("power_on", st.get("state_revision"), target_c=self.a.target)
        except OSError as e:
            raise Abort(f"no answer to power_on ({e})") from e
        if status != 200 or not r.get("ok"):
            self.armed = False
            raise Abort(f"power_on refused: {r.get('error') or status}")
        ttl = (_g(r, "state", "config", "comms_ms") or 300000) / 1000.0
        self.start_beat(r.get("lease_id"), ttl)
        self.event("command", f"power_on target {self.a.target:.0f} C")

    def disarm_other(self, st):
        """If another controller has the heater now, do not send OFF over it."""
        owner = _g(st, "control", "lease", "owner")
        if self.armed and st.get("mode") != "off" and owner and owner != ACTOR["id"]:
            self.armed = False
            self.stop_beat()
            self.event("warn", f"another controller has the heater (owner {owner}). The tool does "
                       "not send OFF. Watch the heater.")

    def safe_off(self):
        """Never raises. Used on each exit path while armed. Stop signals are ignored while it runs,
        so a second Ctrl-C, SIGTERM or SIGHUP cannot stop it before OFF is sent."""
        if not self.armed:
            return True
        old = {}
        if threading.current_thread() is threading.main_thread():
            for sig in STOP_SIGNALS:
                try:
                    old[sig] = signal.signal(sig, signal.SIG_IGN)
                except (ValueError, OSError):
                    pass
        try:
            return self._off_and_check()
        finally:
            for sig, h in old.items():
                signal.signal(sig, h)

    def _off_and_check(self):
        self.stop_beat()
        sent = False
        for _ in range(5):
            try:
                status, r = self.c.command("off")
                if status == 200 and r.get("ok"):
                    self.event("command", "off")
                    sent = True
                    break
            except BaseException:  # noqa: BLE001 - nothing may stop the OFF
                pass
            time.sleep(0.25)
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            try:
                st = self.c.get_json("/api/v2/state")
            except BaseException:  # noqa: BLE001
                st = None
            if st and not _g(st, "heater", "output") and st.get("mode") == "off":
                self.armed = False
                return True
            time.sleep(0.25)
        self.say("\n!!! The heater did not confirm OFF. Switch the heater off at the mains NOW. !!!")
        self.event("error", "OFF not confirmed" + ("" if sent else " (OFF not accepted)"))
        return False

    def stop_heat(self, why):
        """End a heat phase: OFF if the tool armed it, else ask the operator."""
        if self.armed:
            self.safe_off()
            return
        self.event("prompt", f"{why}: stop the heat now (press Power, or Off on the dashboard).")
        self.run_until(lambda s, t: not _g(s, "heater", "output") and s.get("mode") == "off",
                       180.0, "wait off")
        if self.last and (_g(self.last, "heater", "output") or self.last.get("mode") != "off"):
            self.event("warn", "the heater is still on. The capture continues to record.")

    def start_heat(self, wait_s):
        """Start heat at the target: by command, or by the operator. Return when armed."""
        if self.a.allow_control:
            self.power_on()
            return
        self.event("prompt", f"Start heat now: Manual heat at {self.a.target:.0f} C on the dashboard, "
                   "or press On on the heater.")
        r = self.run_until(lambda s, t: (_g(s, "target", "effective_c") or 0) > 0
                           or bool(_g(s, "heater", "output")), wait_s, "wait heat")
        if r != "condition":
            raise Abort(f"no heat started in {wait_s:.0f} s")

    # ---- scenarios ------------------------------------------------------------

    def scenario_idle(self):
        st = self.last
        if st and (st.get("mode") != "off" or _g(st, "heater", "output")):
            self.event("warn", f"the heater is not idle (mode {st.get('mode')})")
        self.run_until(None, self.a.duration, "idle")
        return "duration reached"

    def scenario_first_heat(self):
        self.run_until(None, self.a.baseline, "baseline")
        st = self.last or {}
        ptc, ch = _g(st, "sensors", "ptc", "temperature_c"), _g(st, "sensors", "chamber", "temperature_c")
        if ptc is not None and ch is not None and abs(ptc - ch) > 5:
            self.event("warn", f"not cold: PTC {ptc:.1f} C, chamber {ch:.1f} C. The result is not a "
                       "cold first heat.")
        self.start_heat(self.a.wait)
        r = self.run_until(lambda s, t: bool(_g(s, "heater", "output")), self.a.wait, "wait relay")
        if r != "condition":
            self.stop_heat("no relay ON")
            return f"the relay did not switch on in {self.a.wait:.0f} s"
        self.event("note", "relay on")
        self.run_until(lambda s, t: bool(_g(s, "safety", "fault_latched")), self.a.heat_seconds, "heat")
        self.stop_heat("first heat recorded")
        self.run_until(None, self.a.tail, "tail")
        return "first heat recorded"

    def scenario_step(self):
        self.start_heat(self.a.wait)
        reached = {}

        def done(s, t):
            ch = _g(s, "sensors", "chamber", "temperature_c")
            tgt = _g(s, "target", "effective_c") or self.a.target
            if "t" not in reached and ch is not None and ch >= tgt:
                reached["t"] = t
                self.event("note", f"target reached after {t / 60:.1f} min")
            if not self.armed and s.get("mode") == "off":
                return True
            return "t" in reached and t - reached["t"] >= self.a.hold
        r = self.run_until(done, self.a.duration, "step")
        self.stop_heat("step recorded")
        if self.a.then_cooldown:
            self.scenario_cooldown()
        if r == "time":
            return "maximum duration reached" + ("" if reached else " (target not reached)")
        return "hold time reached" if reached else "the mode changed to off"

    def scenario_cooldown(self):
        if self.last is None:
            self.run_until(lambda s, t: True, 10.0, "first sample")
        st = self.last or {}
        if _g(st, "heater", "output") or st.get("mode") not in ("off", None):
            # The tool sends OFF only to its own heat (stop_heat). Another heat (a print in AUTO,
            # Klipper, a button) belongs to the operator: the tool asks for it.
            self.stop_heat("cooldown capture")
        quiet = {}

        def done(s, t):
            fan = _g(s, "fan", "effective_percent")
            ptc = _g(s, "sensors", "ptc", "temperature_c")
            ch = _g(s, "sensors", "chamber", "temperature_c")
            calm = (not fan and not _g(s, "heater", "output") and ptc is not None and ch is not None
                    and abs(ptc - ch) < 3.0)
            if not calm:
                quiet.pop("t", None)
                return False
            quiet.setdefault("t", t)
            return t - quiet["t"] >= 60
        r = self.run_until(done, self.a.duration, "cooldown")
        return "cool and fan off" if r == "condition" else "maximum duration reached"

    def scenario_soak(self):
        self.run_until(None, self.a.duration, "soak")
        return "duration reached"

    def run(self):
        out = self.open()
        self.say(PROMPTS[self.a.scenario])
        self.event("prompt", PROMPTS[self.a.scenario])
        reason = "unknown"
        try:
            fn = getattr(self, "scenario_" + self.a.scenario.replace("-", "_"))
            reason = fn()
        except Abort as e:
            reason = f"aborted: {e}"
            self.event("stop", reason)
        except KeyboardInterrupt:
            reason = "interrupted"
            self.event("stop", reason)
        finally:
            ok = self.safe_off()
            if not ok:
                reason += "; OFF NOT CONFIRMED"
            self.w.close(stop_reason=reason,
                         ended_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                         duration_s=self.now())
        self.say(f"stop: {reason}")
        self.say(f"next: python3 tools/analyze_capture.py {out}")
        return out, reason


def parse_args(argv):
    p = argparse.ArgumentParser(description="Record a DragonBreath capture scenario.")
    p.add_argument("host", help="device host or IP (or http://host:port)")
    p.add_argument("scenario", choices=sorted(SCENARIOS))
    p.add_argument("--out", default="captures", help="parent directory (default: captures)")
    p.add_argument("--notes", default="", help="free text for the header (board, door, room)")
    p.add_argument("--ambient-c", type=float, help="room temperature from a thermometer")
    p.add_argument("--mains-v", type=float, help="mains voltage at the outlet")
    p.add_argument("--rated-v", type=float, help="rated voltage on the unit label")
    p.add_argument("--rate", type=float, help="state samples per second")
    p.add_argument("--metrics-every", type=float, help="seconds between /metrics snapshots")
    p.add_argument("--duration", type=float, help="seconds: idle/soak length, step/cooldown maximum")
    p.add_argument("--target", type=float, default=None, help="heat target C (30-60)")
    p.add_argument("--hold", type=float, default=1800.0, help="step: seconds to hold after the target")
    p.add_argument("--then-cooldown", action="store_true", help="step: record the cooldown after it")
    p.add_argument("--baseline", type=float, default=60.0, help="first-heat: seconds before heat")
    p.add_argument("--heat-seconds", type=float, default=120.0, help="first-heat: seconds after relay ON")
    p.add_argument("--tail", type=float, default=60.0, help="first-heat: seconds after OFF")
    p.add_argument("--wait", type=float, default=600.0, help="seconds to wait for heat to start")
    p.add_argument("--allow-control", action="store_true",
                   help="let the tool send power_on/off and its heartbeat (needs --token)")
    p.add_argument("--token", default=None,
                   help="control token (only with --allow-control). Prefer --token-file: a command "
                        "line is visible to other users")
    p.add_argument("--token-file", default=None, help="file that holds the control token")
    p.add_argument("--allow-mains-mismatch", action="store_true",
                   help="heat although --mains-v and --rated-v do not agree")
    p.add_argument("--yes", action="store_true", help="do not ask before power_on")
    a = p.parse_args(argv)
    rate, mevery, dur = SCENARIOS[a.scenario]
    a.rate = a.rate or rate
    a.metrics_every = a.metrics_every or mevery
    a.duration = a.duration or dur
    if a.target is None:
        a.target = 50.0 if a.scenario == "step" else 40.0
    if not 0 < a.rate <= 10:
        p.error("--rate must be in (0, 10]")
    if not MIN_TARGET_C <= a.target <= MAX_TARGET_C:
        p.error(f"--target must be {MIN_TARGET_C:.0f}-{MAX_TARGET_C:.0f} C")
    if a.token_file:
        if a.token:
            p.error("give --token or --token-file, not both")
        try:
            with open(a.token_file) as f:
                a.token = f.read().strip()
        except OSError as e:
            p.error(f"--token-file: {e}")
        if not a.token:
            p.error("--token-file is empty")
    if a.allow_control and not a.token:
        p.error("--allow-control needs an explicit --token-file or --token")
    if a.token and not a.allow_control:
        p.error("--token is used only with --allow-control (the capture is read-only otherwise)")
    return a


def _interrupt(*_):
    raise KeyboardInterrupt


def main(argv=None, ask=input):
    a = parse_args(argv)
    old = {}
    for name in ("SIGTERM", "SIGHUP"):
        s = getattr(signal, name, None)
        if s is not None:
            try:
                old[s] = signal.signal(s, _interrupt)
            except ValueError:   # not the main thread
                pass
    try:
        cap = Capturer(a, ask)
        try:
            out, reason = cap.run()
        except Abort as e:
            print(f"capture not started: {e}")
            return 2
    finally:
        for s, h in old.items():
            signal.signal(s, h)
    return 0 if reason in DONE_REASONS else 1


if __name__ == "__main__":
    sys.exit(main())
