#!/usr/bin/env python3
"""End-to-end tests of tools/smoke.py against a mock API v2 device.

Run: python3 -m unittest tests/test_smoke.py

The mock follows components/pb_httpd and pb_policy: request ids, expected
revisions, 32-character leases, a lease TTL equal to comms_ms, a latched
watchdog trip, the persisted fault latch over a reboot, and panic-off.
"""
import contextlib
import importlib.util
import io
import json
import pathlib
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


class MockDevice:
    """Simulated firmware state. tick() is the control loop."""

    def __init__(self):
        self.lock = threading.Lock()
        self.boot_id = uuid.uuid4().hex
        self.rev = 1
        self.mode, self.source, self.target, self.output = "off", "boot", 0.0, False
        self.chamber, self.chamber_status = 25.0, "ok"
        self.fault, self.reason, self.persisted = False, None, False
        self.lease, self.deadline = None, 0.0
        self.comms_ms, self.comms_min = 300000, 1000
        self.metrics = True
        self.broken_watchdog = False   # simulate a firmware that never trips
        self.panic_after = None        # seconds after relay on, then a button panic-off
        self.on_since = 0.0
        self.commands = []

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
            "control": {"lease": {"active": self.lease is not None,
                                  "owner": "smoke-test" if self.lease else None,
                                  "expires_in_ms": 0}},
            "safety": {"fault_latched": self.fault, "inhibited": False, "reason": self.reason},
        }

    def _off(self, source):
        self.mode, self.source, self.target, self.lease = "off", source, 0.0, None
        self.rev += 1

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
            self.output = demand

    def reboot(self):
        with self.lock:
            self.boot_id = uuid.uuid4().hex
            self.mode, self.source, self.target, self.output, self.lease = "off", "boot", 0.0, False, None
            if self.fault and self.persisted:
                self.reason = "safety trip"
            else:
                self.fault, self.reason = False, None
            self.rev = 1

    def metrics_text(self):
        on = int(self.output)
        return "\n".join([
            "# HELP dragonbreath_build_info Firmware version.",
            "# TYPE dragonbreath_build_info gauge",
            'dragonbreath_build_info{version="9.9.9-mock",idf_version="v5.3"} 1',
            "# TYPE dragonbreath_uptime_seconds gauge",
            "dragonbreath_uptime_seconds 12.5",
            "# TYPE dragonbreath_temperature_celsius gauge",
            f'dragonbreath_temperature_celsius{{sensor="chamber"}} {self.chamber}',
            'dragonbreath_temperature_celsius{sensor="ptc"} 30.0',
            "# TYPE dragonbreath_sensor_status gauge",
            'dragonbreath_sensor_status{sensor="chamber",status="ok"} 1',
            "# TYPE dragonbreath_heater_relay_on gauge",
            f"dragonbreath_heater_relay_on {on}",
            "# TYPE dragonbreath_fault_latched gauge",
            f"dragonbreath_fault_latched {int(self.fault)}",
            "# TYPE dragonbreath_mode gauge",
            f'dragonbreath_mode{{mode="off"}} {int(self.mode == "off")}',
            "",
        ])

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
            if self.fault:
                return 409, {"ok": False, "error": "fault_latched"}
            self.mode, self.source, self.target = "power_on", actor["kind"], float(cmd["target_c"])
            self.lease = lease = uuid.uuid4().hex
            self.deadline = time.monotonic() + self.comms_ms / 1000
            self.rev += 1
        elif name == "clear_fault":
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

    def send(self, status, obj, ctype="application/json"):
        raw = obj.encode() if isinstance(obj, str) else json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        dev = self.server.dev
        with dev.lock:
            if self.path == "/api/v2/state":
                return self.send(200, dev.snapshot())
            if self.path == "/api/v2/health":
                return self.send(200, {"api_version": 2, "boot_id": dev.boot_id, "uptime_ms": 12500,
                                       "free_heap_bytes": 90000, "wifi_rssi_dbm": -60})
            if self.path == "/settings":
                return self.send(200, dev.settings())
            if self.path == "/metrics" and dev.metrics:
                return self.send(200, dev.metrics_text(), "text/plain; version=0.0.4")
        self.send(404, "Nothing matches the given URI", "text/html")

    def do_POST(self):
        dev = self.server.dev
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        if self.headers.get("X-DragonBreath-Auth") != TOKEN:
            return self.send(403, {"ok": False, "error": "auth_failed"})
        url = urllib.parse.urlsplit(self.path)
        reboot = False
        with dev.lock:
            if url.path == "/api/v2/command":
                status, out = dev.command(json.loads(raw or b"{}"))
            elif url.path == "/api/v2/heartbeat":
                status, out = dev.heartbeat(json.loads(raw or b"{}"))
            elif url.path == "/settings":
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
        self.send(status, out)
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
        self.server.shutdown()
        self.server.server_close()

    def _ticker(self):
        while not self.stop.wait(0.05):
            self.dev.tick()

    def run_tool(self, *args, answers=("y",)):
        answers = list(answers)

        def ask(prompt):
            self.prompts.append(prompt)
            answer = answers.pop(0) if len(answers) > 1 else answers[0]
            return answer(prompt) if callable(answer) else answer

        host = f"127.0.0.1:{self.server.server_address[1]}"
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = smoke.main([host, "--token", TOKEN, *args], ask=ask)
        return code, out.getvalue()

    def assert_safe_end(self):
        time.sleep(0.15)
        self.assertEqual(self.dev.mode, "off")
        self.assertFalse(self.dev.output)
        self.assertEqual(self.dev.comms_ms, 300000)

    def test_full_run_passes(self):
        def answer(prompt):   # the "operator" holds a button in step 5
            if "Step 5" in prompt:
                self.dev.panic_after = 0.3
            return "r" if "Step 4" in prompt else "y"
        code, out = self.run_tool(answers=(answer,))
        self.assertEqual(code, 0, out)
        for n in range(1, 6):
            self.assertRegex(out, rf"PASS\s+{n} ")
        self.assertIn("RESULT: PASS", out)
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

    def test_sensor_fault_fails_step1_and_stops(self):
        self.dev.chamber_status = "open"
        code, out = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("sensor not ok: chamber", out)
        self.assertNotIn("power_on", self.dev.commands)

    def test_latched_fault_needs_allow_fault(self):
        self.dev.latch("chamber over-temp", "safety", persist=True)
        code, out = self.run_tool("--steps", "1")
        self.assertEqual(code, 1)
        self.assertIn("--allow-fault", out)
        code, out = self.run_tool("--steps", "1", "--allow-fault")
        self.assertEqual(code, 0, out)

    def test_decline_skips_without_heat(self):
        code, out = self.run_tool("--steps", "2,3,5", answers=("n",))
        self.assertEqual(code, 0, out)
        self.assertEqual(out.count("[SKIP]"), 3)
        self.assertEqual(self.dev.commands, [])
        self.assert_safe_end()

    def test_other_controller_is_not_touched(self):
        self.dev.mode, self.dev.source = "auto", "klipper"
        code, out = self.run_tool("--steps", "2")
        self.assertEqual(code, 1)
        self.assertIn("stop the other controller first", out)
        self.assertEqual(self.dev.commands, [])
        self.assertEqual(self.dev.mode, "auto")

    def test_refuses_hot_chamber(self):
        self.dev.chamber = 39.0
        code, out = self.run_tool("--steps", "2")
        self.assertEqual(code, 1)
        self.assertIn("above target-2", out)
        self.assertNotIn("power_on", self.dev.commands)

    def test_target_above_50_is_refused(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            smoke.parse_args(["host", "--target", "55"])

    def test_watchdog_that_never_trips_fails_and_cleans_up(self):
        self.dev.broken_watchdog = True
        code, out = self.run_tool("--steps", "3", "--trip-margin", "0.5")
        self.assertEqual(code, 1)
        self.assertIn("no fault latched", out)
        self.assertIn("comms_ms restored to 300000", out)
        self.assert_safe_end()

    def test_step3_alone_clears_its_fault(self):
        code, out = self.run_tool("--steps", "3")
        self.assertEqual(code, 0, out)
        self.assertIn("tripped after", out)
        self.assertFalse(self.dev.fault)
        self.assert_safe_end()

    def test_lost_persistence_fails(self):
        self.dev.latch(LEASE_REASON, "watchdog", persist=False)
        code, out = self.run_tool("--steps", "4", answers=("r",))
        self.assertEqual(code, 1)
        self.assertIn("NOT latched after the reboot", out)

    def test_ctrl_c_while_heating_sends_off(self):
        with mock.patch.object(smoke.Smoke, "wait_relay_on", side_effect=KeyboardInterrupt):
            code, out = self.run_tool("--steps", "2")
        self.assertNotEqual(code, 0)
        self.assertIn("interrupted", out)
        self.assertEqual(self.dev.commands[-1], "off")
        self.assert_safe_end()

    def test_slow_off_fails(self):
        real_command = self.dev.command

        def ignore_off(body):   # a firmware that accepts OFF but keeps the relay on
            if body["command"]["name"] == "off" and self.dev.output:
                self.dev.commands.append("off-ignored")
                return 200, {"ok": True, "request_id": body["request_id"], "state": self.dev.snapshot()}
            return real_command(body)
        self.dev.command = ignore_off
        code, out = self.run_tool("--steps", "2")
        self.assertEqual(code, 1)
        self.assertIn("still true", out)


if __name__ == "__main__":
    unittest.main()
