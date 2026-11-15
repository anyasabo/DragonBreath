"""End-to-end tests of tools/smoke.py against a mock API v2 device.

Run: python3 -m unittest tests/test_smoke.py

The mock follows components/pb_httpd and pb_policy: auth (token or any non-empty
header), request ids, expected revisions, 32-character leases, a lease TTL equal
to comms_ms (minimum 10 s), a 500 ms control tick that a command wakes, the
latched watchdog trip, the persisted fault over a reboot (with ~2 s offline),
AUTO override, panic-off and the clear_fault errors.
"""
import contextlib
import importlib.util
import io
import json
import os
import pathlib
import signal
import threading
import time
import unittest
import urllib.parse
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("dragonbreath_smoke", ROOT / "tools" / "smoke.py")
smoke = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(smoke)

TOKEN = "tok"
LEASE_REASON = "Web controller lease expired - browser tab may be backgrounded"
TICK_S = 0.5


class MockDevice:
    """Simulated firmware state. tick() is the control loop."""

    def __init__(self):
        self.lock = threading.Lock()
        self.wake = threading.Event()
        self.token = TOKEN
        self.boot_id = uuid.uuid4().hex
        self.rev = 1
        self.mode, self.source, self.target, self.output = "off", "boot", 0.0, False
        self.resume_mode = "off"
        self.chamber, self.chamber_status = 25.0, "ok"
        self.fault, self.reason, self.persisted = False, None, False
        self.inhibited, self.persist_fail = False, False
        self.lease, self.deadline = None, 0.0
        self.comms_ms, self.comms_min = 300000, 10000
        self.metrics = True
        self.down_until = 0.0
        self.broken_watchdog = False   # a firmware that never trips
        self.relay_stuck = False       # a relay that stays on after OFF
        self.break_next_off = False    # apply OFF, then cut the response
        self.settings_fail_after = None
        self.panic_after = None        # seconds after relay on, then a button panic-off
        self.on_since = 0.0
        self.commands = []

    def lease_active(self):
        return self.lease is not None and time.monotonic() < self.deadline

    def snapshot(self):
        return {
            "api_version": 2, "device_id": "dragonbreath-mock", "boot_id": self.boot_id,
            "firmware": "9.9.9-mock", "state_revision": self.rev, "mode": self.mode,
            "source": self.source,
            "target": {"requested_c": self.target, "effective_c": self.target, "maximum_c": 70.0},
            "config": {"max": 70.0, "max_abs": 70.0, "comms_ms": self.comms_ms},
            "heater": {"demand": self.mode == "power_on" and not self.fault, "output": self.output},
            "sensors": {
                "chamber": {"temperature_c": self.chamber if self.chamber_status == "ok" else None,
                            "status": self.chamber_status},
                "ptc": {"temperature_c": 30.0, "status": "ok"},
            },
            "control": {"lease": {"active": self.lease_active(),
                                  "owner": "smoke-test" if self.lease_active() else None,
                                  "expires_in_ms": 0}},
            "safety": {"fault_latched": self.fault, "inhibited": self.inhibited, "reason": self.reason},
        }

    def _off(self, source):
        self.mode, self.source, self.target, self.lease = "off", source, 0.0, None
        self.resume_mode = "off"
        self.rev += 1
        self.wake.set()

    def latch(self, reason, source, persist):
        self.fault, self.reason, self.persisted = True, reason, persist
        self._off(source)

    def tick(self):
        with self.lock:
            now = time.monotonic()
            if (self.mode == "power_on" and self.lease and now >= self.deadline
                    and not self.broken_watchdog):
                self.latch(LEASE_REASON, "watchdog", persist=True)
            if self.panic_after is not None and self.output and now - self.on_since >= self.panic_after:
                self.panic_after = None
                self.latch("button panic-off", "button", persist=False)
            demand = self.mode == "power_on" and not self.fault and self.chamber < self.target - 1
            if demand and not self.output:
                self.on_since = now
            self.output = demand or (self.output and self.relay_stuck)

    def reboot(self):
        with self.lock:
            self.down_until = time.monotonic() + 2.0
            self.boot_id = uuid.uuid4().hex
            self.mode, self.source, self.target, self.output, self.lease = "off", "boot", 0.0, False, None
            self.inhibited = False
            if self.fault and self.persisted:
                self.reason = "safety trip"
            else:
                self.fault, self.reason = False, None
            self.rev = 1

    def metrics_text(self):
        return "\n".join([
            "# HELP dragonbreath_build_info Firmware version.",
            "# TYPE dragonbreath_build_info gauge",
            'dragonbreath_build_info{version="9.9.9-mock",idf_version="v5.3"} 1',
            "dragonbreath_uptime_seconds 12.5",
            f'dragonbreath_temperature_celsius{{sensor="chamber"}} {self.chamber}',
            'dragonbreath_sensor_status{sensor="chamber",status="ok"} 1',
            f"dragonbreath_heater_relay_on {int(self.output)}",
            f"dragonbreath_fault_latched {int(self.fault or self.inhibited)}",
            f'dragonbreath_mode{{mode="off"}} {int(self.mode == "off")}',
            "",
        ])

    def auth_ok(self, header):
        return bool(header) and (self.token is None or header == self.token)

    # ---- request handlers: return (status, JSON object) ----------------------

    def command(self, body):
        if body.get("api_version") != 2:
            return 400, {"ok": False, "error": "unsupported_api_version"}
        rid, actor, cmd = body.get("request_id"), body.get("actor") or {}, body.get("command") or {}
        if not rid or actor.get("kind") not in ("web", "klipper") or not actor.get("id") or "name" not in cmd:
            return 400, {"ok": False, "error": "invalid_command"}
        name, rev = cmd["name"], body.get("expected_revision")
        self.commands.append(name)
        lease = None
        if name == "off":
            self._off(actor["kind"])
        elif not isinstance(rev, int):
            return 400, {"ok": False, "error": "invalid_command"}
        elif rev != self.rev:
            return 409, {"ok": False, "error": "revision_conflict"}
        elif name == "power_on":
            if not isinstance(cmd.get("target_c"), (int, float)) or cmd["target_c"] < 30:
                return 400, {"ok": False, "error": "invalid_command"}
            if self.inhibited:
                return 409, {"ok": False, "error": "inhibited"}
            if self.fault:
                return 409, {"ok": False, "error": "fault_latched"}
            self.resume_mode = "auto" if self.mode == "auto" else "off"
            self.mode, self.source, self.target = "power_on", actor["kind"], float(cmd["target_c"])
            self.lease = lease = uuid.uuid4().hex
            self.deadline = time.monotonic() + self.comms_ms / 1000
            self.rev += 1
            self.wake.set()
        elif name == "clear_fault":
            if self.inhibited:
                return 409, {"ok": False, "error": "inhibited"}
            if self.persist_fail:
                return 500, {"ok": False, "error": "persist_failed"}
            self.fault, self.reason, self.persisted = False, None, False
            self._off(actor["kind"])
        else:
            return 400, {"ok": False, "error": "invalid_command"}
        out = {"ok": True, "request_id": rid, "state": self.snapshot()}
        if lease:
            out["lease_id"] = lease
        return 200, out

    def heartbeat(self, body):
        lease = body.get("lease_id")
        if body.get("api_version") != 2 or not isinstance(lease, str) or len(lease) != 32:
            return 400, {"ok": False, "error": "invalid_command"}
        now = time.monotonic()
        if lease != self.lease or self.mode != "power_on" or now >= self.deadline:
            return 409, {"ok": False, "error": "stale_lease"}
        self.deadline = now + self.comms_ms / 1000
        return 200, {"ok": True, "expires_in_ms": self.comms_ms, "state_revision": self.rev}

    def settings(self):
        return {"max": 70.0, "comms_ms": self.comms_ms, "comms_ms_min": self.comms_min,
                "comms_ms_max": 300000}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, status, obj, ctype="application/json", cut=False):
        raw = obj.encode() if isinstance(obj, str) else json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw[:10] if cut else raw)
        if cut:
            self.close_connection = True

    def offline(self):
        if time.monotonic() < self.server.dev.down_until:
            self.close_connection = True   # no response: the client sees a dropped connection
            return True
        return False

    def do_GET(self):
        dev = self.server.dev
        if self.offline():
            return
        with dev.lock:
            if self.path == "/api/v2/state":
                return self.send(200, dev.snapshot())
            if self.path == "/api/v2/health":
                return self.send(200, {"api_version": 2, "boot_id": dev.boot_id, "uptime_ms": 12500,
                                       "free_heap_bytes": 90000, "wifi_rssi_dbm": -60})
            if self.path == "/api/v2/console":
                if not dev.auth_ok(self.headers.get("X-DragonBreath-Auth")):
                    return self.send(403, {"ok": False, "error": "auth_failed"})
                return self.send(200, "I (123) boot: mock\n", "text/plain")
            if self.path == "/settings":
                return self.send(200, dev.settings())
            if self.path == "/metrics" and dev.metrics:
                return self.send(200, dev.metrics_text(), "text/plain; version=0.0.4")
        self.send(404, "Nothing matches the given URI", "text/html")

    def do_POST(self):
        dev = self.server.dev
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        if self.offline():
            return
        if not dev.auth_ok(self.headers.get("X-DragonBreath-Auth")):
            return self.send(403, {"ok": False, "error": "auth_failed"})
        url = urllib.parse.urlsplit(self.path)
        reboot = cut = False
        with dev.lock:
            if url.path == "/api/v2/command":
                status, out = dev.command(json.loads(raw or b"{}"))
                if dev.commands and dev.commands[-1] == "off" and dev.break_next_off:
                    dev.break_next_off, cut = False, True
            elif url.path == "/api/v2/heartbeat":
                status, out = dev.heartbeat(json.loads(raw or b"{}"))
            elif url.path == "/settings":
                if dev.settings_fail_after is not None:
                    if dev.settings_fail_after <= 0:
                        return self.send(500, "Internal Server Error", "text/html")
                    dev.settings_fail_after -= 1
                q = urllib.parse.parse_qs(url.query)
                if "comms_ms" in q:
                    dev.comms_ms = max(dev.comms_min, min(300000, int(q["comms_ms"][0])))
                status, out = 200, dev.settings()
            elif url.path == "/api/v2/restart":
                if dev.mode != "off" or dev.output:
                    status, out = 409, {"ok": False, "error": "heater_active"}
                else:
                    status, out, reboot = 200, {"ok": True, "message": "restarting"}, True
            else:
                status, out = 404, {"ok": False}
        self.send(status, out, cut=cut)
        if reboot:
            threading.Timer(0.3, dev.reboot).start()


class SmokeTest(unittest.TestCase):
    def setUp(self):
        self.dev = MockDevice()
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.dev = self.dev
        self.stop = threading.Event()
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        threading.Thread(target=self._ticker, daemon=True).start()
        self.prompts = []

    def tearDown(self):
        self.stop.set()
        self.dev.wake.set()
        self.server.shutdown()
        self.server.server_close()

    def _ticker(self):
        while not self.stop.is_set():
            self.dev.wake.wait(TICK_S)
            self.dev.wake.clear()
            self.dev.tick()

    def run_tool(self, *args, answers=("y",), token=TOKEN):
        answers = list(answers)

        def ask(prompt):
            self.prompts.append(prompt)
            answer = answers.pop(0) if len(answers) > 1 else answers[0]
            return answer(prompt) if callable(answer) else answer

        host = f"127.0.0.1:{self.server.server_address[1]}"
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = smoke.main([host, "--token", token, *args], ask=ask)
        return code, out.getvalue()

    def assert_safe_end(self, comms_ms=300000):
        time.sleep(0.1)
        self.assertEqual(self.dev.mode, "off")
        self.assertFalse(self.dev.output)
        self.assertEqual(self.dev.comms_ms, comms_ms)

    def test_full_run_passes(self):
        def answer(prompt):   # the "operator" holds a button in step 5
            if "Step 5" in prompt:
                self.dev.panic_after = 0.3
            return "r" if "Step 4" in prompt else "y"
        code, out = self.run_tool(answers=(answer,))
        self.assertEqual(code, 0, out)
        for n in range(1, 6):
            self.assertRegex(out, rf"PASS\s+{n} ")
        self.assertIn("no fault latched", out)
        self.assertNotIn("mains", out)
        self.assertFalse(self.dev.fault)
        self.assert_safe_end()

    def test_metrics_absent_is_a_note(self):
        self.dev.metrics = False
        code, out = self.run_tool("--steps", "1")
        self.assertEqual(code, 0, out)
        self.assertIn("/metrics not present (404)", out)

    def test_metrics_missing_family_fails(self):
        self.dev.metrics_text = lambda: "dragonbreath_uptime_seconds 1\n"
        code, out = self.run_tool("--steps", "1")
        self.assertEqual(code, 1)
        self.assertIn("/metrics misses dragonbreath_build_info", out)

    def test_parse_metrics(self):
        m = smoke.parse_metrics('# TYPE a gauge\na{x="1",y="b c"} 2\nb_total 3.5 1700000000\n')
        self.assertEqual(m["a"], [({"x": "1", "y": "b c"}, 2.0)])
        self.assertEqual(m["b_total"], [({}, 3.5)])

    def test_wrong_token_is_not_an_emergency(self):
        code, out = self.run_tool(token="wrong")
        self.assertEqual(code, 1)
        self.assertIn("wrong --token", out)
        self.assertIn("Run again with the correct --token.", out)
        self.assertNotIn("mains", out)
        self.assertEqual(self.dev.commands, [])

    def test_no_device_token_accepts_any_header(self):
        self.dev.token = None
        code, out = self.run_tool("--steps", "1", token="anything")
        self.assertEqual(code, 0, out)
        self.assertIn("token ok", out)

    def test_sensor_fault_fails_step1_and_stops(self):
        self.dev.chamber_status = "open"
        code, out = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("sensor not ok: chamber", out)
        self.assertEqual(self.dev.commands, [])

    def test_latched_fault_needs_allow_fault(self):
        self.dev.latch("chamber over-temp", "safety", persist=True)
        code, out = self.run_tool("--steps", "1")
        self.assertEqual(code, 1)
        self.assertIn("--allow-fault", out)
        code, out = self.run_tool("--steps", "1", "--allow-fault")
        self.assertEqual(code, 0, out)
        self.assertIn("a fault stays latched: chamber over-temp", out)

    def test_decline_skips_without_heat(self):
        code, out = self.run_tool("--steps", "2,3,5", answers=("n",))
        self.assertEqual(code, 0, out)
        self.assertEqual(out.count("[SKIP]"), 3)
        self.assertEqual(self.dev.commands, [])
        self.assert_safe_end()

    def test_ctrl_c_at_a_prompt_stops_the_run(self):
        code, out = self.run_tool("--steps", "2,3", answers=(mock.Mock(side_effect=KeyboardInterrupt),))
        self.assertEqual(code, 1)
        self.assertEqual(len(self.prompts), 1)
        self.assertNotIn("Step 3", out)
        self.assertEqual(self.dev.commands, [])

    def test_other_controller_is_not_touched(self):
        self.dev.mode, self.dev.source = "auto", "klipper"
        code, out = self.run_tool("--steps", "2")
        self.assertEqual(code, 1)
        self.assertIn("Stop the other controller first.", out)
        self.assertEqual(self.dev.commands, [])
        self.assertEqual(self.dev.mode, "auto")

    def test_revision_race_gives_no_heat_and_no_off(self):
        def arm_auto(prompt):   # a button arms AUTO between precheck and power_on
            with self.dev.lock:
                self.dev.mode, self.dev.source = "auto", "button"
                self.dev.rev += 1
            return "y"
        code, out = self.run_tool("--steps", "2", answers=(arm_auto,))
        self.assertEqual(code, 1)
        self.assertIn("the device state changed", out)
        self.assertEqual(self.dev.commands, ["power_on"])
        self.assertEqual(self.dev.mode, "auto")

    def test_refuses_hot_chamber(self):
        self.dev.chamber = 39.0
        code, out = self.run_tool("--steps", "2")
        self.assertEqual(code, 1)
        self.assertIn("above target-2", out)
        self.assertEqual(self.dev.commands, [])

    def test_target_above_50_is_refused(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            smoke.parse_args(["host", "--target", "55"])

    def test_network_error_mid_off_recovers(self):
        self.dev.break_next_off = True
        code, out = self.run_tool("--steps", "2")
        self.assertEqual(code, 0, out)
        self.assertGreaterEqual(self.dev.commands.count("off"), 2)
        self.assertNotIn("mains", out)
        self.assert_safe_end()

    def test_relay_stuck_on_after_off_warns(self):
        self.dev.relay_stuck = True
        code, out = self.run_tool("--steps", "2", "--off-verify", "1")
        self.assertEqual(code, 1)
        self.assertIn("Switch the heater off at the mains NOW", out)
        self.assertRegex(out, r"FAIL\s+OFF check")
        self.assertIn("== Final device state ==\n  mode off  relay on=True", out)

    def test_watchdog_that_never_trips_fails_and_cleans_up(self):
        self.dev.broken_watchdog = True
        code, out = self.run_tool("--steps", "3", "--trip-margin", "0.5")
        self.assertEqual(code, 1)
        self.assertIn("no fault latched", out)
        self.assertIn("comms_ms restored to 300000", out)
        self.assert_safe_end()

    def test_failed_restore_is_its_own_fail(self):
        self.dev.settings_fail_after = 1
        code, out = self.run_tool("--steps", "3")
        self.assertEqual(code, 1)
        self.assertRegex(out, r"PASS\s+3 comms watchdog")
        self.assertRegex(out, r"FAIL\s+comms_ms restore")
        self.assertIn("dashboard Safety settings", out)
        self.assertFalse(self.dev.fault)     # step 3 alone clears its own trip
        self.assert_safe_end(comms_ms=10000)

    def test_lost_persistence_fails(self):
        self.dev.latch(LEASE_REASON, "watchdog", persist=False)
        code, out = self.run_tool("--steps", "4", answers=("r",))
        self.assertEqual(code, 1)
        self.assertIn("NOT latched after the reboot", out)

    def test_foreign_fault_is_cleared_only_on_request(self):
        self.dev.latch("chamber over-temp", "safety", persist=True)
        code, out = self.run_tool("--steps", "4", answers=("r", "n"))
        self.assertEqual(code, 0, out)
        self.assertIn("did not cause the fault", self.prompts[-1])
        self.assertIn("left latched", out)
        self.assertTrue(self.dev.fault)

    def test_clear_persist_failed_is_explained(self):
        self.dev.latch("chamber over-temp", "safety", persist=True)
        self.dev.persist_fail = True
        code, out = self.run_tool("--steps", "4", answers=("r", "y"))
        self.assertEqual(code, 1)
        self.assertIn("could not save to NVS", out)

    def test_sigterm_while_heating_sends_off(self):
        def term():
            os.kill(os.getpid(), signal.SIGTERM)
            time.sleep(2)
        with mock.patch.object(smoke.Smoke, "wait_relay_on", side_effect=term):
            code, out = self.run_tool("--steps", "2")
        self.assertEqual(code, 1)
        self.assertIn("interrupted", out)
        self.assertEqual(self.dev.commands[-1], "off")
        self.assertEqual(signal.getsignal(signal.SIGTERM), signal.SIG_DFL)
        self.assert_safe_end()


if __name__ == "__main__":
    unittest.main()
