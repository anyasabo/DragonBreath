"""Tests of the capture kit: tools/ntc_model.py, priors.py, synth_capture.py,
analyze_capture.py, replay_capture.py and capture.py (against a mock API v2 device).

Run: python3 -m unittest tests/test_capture_tools.py
Standard library only.
"""
import contextlib
import io
import json
import math
import os
import pathlib
import re
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import analyze_capture  # noqa: E402
import capture  # noqa: E402
import dbcapture_common as common  # noqa: E402
import heater_model as hm  # noqa: E402
import ntc_model as ntc  # noqa: E402
import priors  # noqa: E402
import replay_capture  # noqa: E402
import synth_capture  # noqa: E402


# ------------------------------------------------------------------- NTC port

class NtcPortTest(unittest.TestCase):
    def test_table_matches_firmware_source(self):
        src = (ROOT / "components" / "pb_ntc" / "pb_ntc.c").read_text()
        body = re.search(r"PB_RT_R_KOHM\[\]\s*=\s*\{(.*?)\};", src, re.S).group(1)
        body = re.sub(r"//[^\n]*", "", body)
        table = tuple(float(x.strip().rstrip("f")) for x in body.split(",") if x.strip())
        self.assertEqual(table, ntc.RT_R_KOHM)
        base = int(re.search(r"#define PB_RT_TEMP_BASE\s+(\d+)", src).group(1))
        self.assertEqual(base, ntc.RT_TEMP_BASE)
        hdr = (ROOT / "components" / "pb_ntc" / "include" / "pb_ntc.h").read_text()
        self.assertIn(f"#define PB_NTC_VSUPPLY_V      {ntc.VSUPPLY_V}f", hdr)

    def test_values_of_the_c_host_test(self):
        # tests/pb_ntc_chain_host_test.c (test/heater-control-tick): 1650 mV is Vsupply/2,
        # so Rntc = Rref: 82 kOhm -> 31.6 C, 33 kOhm -> 54.17 C.
        self.assertAlmostEqual(ntc.mv_to_temp_c(1650, 82), 31.6, delta=0.1)
        self.assertAlmostEqual(ntc.mv_to_temp_c(1650, 33), 54.17, delta=0.1)

    def test_table_points_clamps_and_open(self):
        for i, r in enumerate(ntc.RT_R_KOHM):
            self.assertAlmostEqual(ntc.r_to_temp_c(r), ntc.RT_TEMP_BASE + i, places=6)
        self.assertEqual(ntc.r_to_temp_c(500.0), 12.0)
        self.assertEqual(ntc.r_to_temp_c(1.0), 125.0)
        self.assertTrue(math.isnan(ntc.mv_to_temp_c(0, 82)))
        self.assertTrue(math.isnan(ntc.mv_to_temp_c(3300, 82)))
        self.assertFalse(math.isnan(ntc.mv_to_temp_c(3299, 82)))

    def test_monotonic_and_round_trip(self):
        prev = 200.0                      # low-side NTC: a higher voltage is colder
        for mv in range(100, 3300, 25):
            t = ntc.mv_to_temp_c(mv, 82)
            self.assertLessEqual(t, prev)
            prev = t
        for t in (12.5, 25.0, 47.3, 99.9, 124.5):
            for rref in (82, 33):
                self.assertAlmostEqual(ntc.mv_to_temp_c(ntc.temp_to_mv(t, rref), rref), t, places=4)

    def test_reinterpret_and_implied_rref(self):
        t_read = ntc.reinterpret_c(25.0, 82, 33)       # true 25 C, divider 82 k, firmware 33 k
        self.assertGreater(t_read, 40)
        self.assertAlmostEqual(ntc.reinterpret_c(t_read, 33, 82), 25.0, delta=0.05)
        self.assertAlmostEqual(ntc.implied_rref_kohm(t_read, 25.0, 33), 82, delta=1)
        self.assertIsNone(ntc.implied_rref_kohm(12.0, 25.0, 82))


class HeaterModelConstantsTest(unittest.TestCase):
    def test_constants_match_pb_heater_h(self):
        h = (ROOT / "components" / "pb_heater" / "include" / "pb_heater.h").read_text()

        def c(name):
            return float(re.search(rf"#define {name}\s+([0-9.]+)f", h).group(1))
        self.assertEqual(c("PB_HEATER_PTC_CUTOFF_C"), hm.PTC_CUTOFF_C)
        self.assertEqual(c("PB_HEATER_CHAMBER_MAX_C"), hm.CHAMBER_MAX_C)
        self.assertEqual(c("PB_HEATER_HYSTERESIS_C"), hm.HYSTERESIS_C)
        self.assertEqual((c("PB_HEATER_PTC_FOLDBACK_CUT_82K_C"), c("PB_HEATER_PTC_FOLDBACK_RESUME_82K_C")),
                         hm.FOLDBACK_82K)
        self.assertEqual((c("PB_HEATER_PTC_FOLDBACK_CUT_33K_C"), c("PB_HEATER_PTC_FOLDBACK_RESUME_33K_C")),
                         hm.FOLDBACK_33K)
        self.assertEqual(c("PB_HEATER_PTC_FOLDBACK_BAND_C"), hm.FOLDBACK_BAND_C)

    def test_first_heat_eval(self):
        self.assertEqual(hm.fh_eval(11999, 100, 0), "wait")
        self.assertEqual(hm.fh_eval(12000, 30, 14), "pass")
        self.assertEqual(hm.fh_eval(12000, 30, 15), "wait")
        self.assertEqual(hm.fh_eval(12000, 20, 30), "crossed")
        self.assertEqual(hm.fh_eval(30000, 10, 0), "no_rise")
        self.assertEqual(hm.c_to_dc(2.95), 30)
        self.assertEqual(hm.c_to_dc(-0.05), -1)


# ------------------------------------------------------------------- priors

class PriorsTest(unittest.TestCase):
    def test_issue_122_ratio_and_hypotheses(self):
        ch = sum(priors.ISSUE_122_IDLE["chamber_c"]) / 2
        ptc = sum(priors.ISSUE_122_IDLE["ptc_c"]) / 2
        inv = priors.inverse(ch, ptc, None, 82)
        p = inv["channels"]["ptc"]
        self.assertAlmostEqual(p["ratio"], 2.70, delta=0.05)     # the coordinator estimate: about 2.7
        self.assertGreater(p["ratio"], 82 / 33)                   # more than the Rref ratio 2.48
        text = " ".join(p["fits"])
        self.assertNotIn("wrong Rref: divider", text)            # 222 k is no board value
        self.assertNotIn("mirror Rref", text)                    # only one channel deviates
        self.assertIn("not an Rref error", text)
        self.assertIn("R25 0.37", text)
        self.assertIn("parallel leak", text)
        self.assertIn("no chain error", " ".join(inv["channels"]["chamber"]["fits"]))

    def test_one_channel_off_with_ambient_is_not_rref(self):
        inv = priors.inverse(22.0, priors.display_c(22.0, 33, 82), 22.0, 33)
        self.assertFalse(inv["both_channels_same_ratio"])
        self.assertNotIn("wrong Rref: divider", " ".join(inv["channels"]["ptc"]["fits"]))

    def test_wrong_rref_fits(self):
        shown = priors.display_c(22.0, 33, 82)            # firmware 33 k on an 82 k divider
        inv = priors.inverse(shown, shown, 22.0, 33)
        self.assertIn("wrong Rref: divider 82 k", " ".join(inv["channels"]["ptc"]["fits"]))

    def test_forensics_rows(self):
        rows = {r[0]: r[1] for r in priors.forensics(22.0, 82)}
        self.assertAlmostEqual(rows["correct chain"], 22.0, delta=0.01)
        self.assertEqual(rows["wrong Rref: firmware 82 k, divider 33 k"], 12.0)    # table clamp
        self.assertGreater(rows["wrong Rref: firmware 33 k, divider 82 k"], 40)
        self.assertIsNone(rows["open thermistor"])

    def test_energy_outlet_mains_enclosure(self):
        fh = priors.first_heat()
        self.assertAlmostEqual(fh["rows"][0]["rate_c_s"], 300 / 90, places=3)
        self.assertGreater(min(r["margin_x"] for r in fh["rows"]), 5)
        self.assertAlmostEqual(fh["issue122_rate_c_s"], 34.9 / 24.2, places=3)
        out = priors.outlet()
        self.assertAlmostEqual(out[1]["rise_c"], 300 / (0.2 / 60 * 1.2 * 1005), places=3)
        self.assertTrue(out[0]["capped"])
        self.assertAlmostEqual(priors.mains(120, 230)["ptc_power_w"], 300 * (120 / 230) ** 2)
        self.assertIn("slow", priors.mains(120, 230)["fan"])
        en = priors.enclosure()
        self.assertEqual(en["gain_c_per_duty"], (25.0, 37.5))
        minutes = [r["minutes"] for r in en["rows"] if r["bed_w"] > 0 and r["minutes"]]
        self.assertTrue(any(40 <= m <= 80 for m in minutes))    # "about 1 h" with bed + heater
        self.assertIn("2.5 h", "\n".join(priors.render(idle=(25.5, 49.6))))


# ------------------------------------------------------------------- fits

class FitTest(unittest.TestCase):
    def test_exp_fit_recovers_tau(self):
        ts = [i * 0.5 for i in range(240)]
        ys = [30 + 40 * math.exp(-t / 17.0) for t in ts]
        f = analyze_capture.exp_fit(ts, ys)
        self.assertAlmostEqual(f["tau_s"], 17.0, delta=0.2)
        self.assertAlmostEqual(f["asymptote_c"], 30.0, delta=0.2)

    def test_fopdt_fit_recovers_model(self):
        dt, n = 5.0, 1200
        u = [0.0] * 20 + [1.0] * (n - 20)
        K, tau, theta = 30.0, 1500.0, 40.0
        y, x = [], 0.0
        a = math.exp(-dt / tau)
        sh = int(theta / dt)
        for i in range(n):
            ui = u[i - sh] if i >= sh else 0.0
            x = a * x + (1 - a) * ui
            y.append(22.0 + K * x)
        f = analyze_capture.fopdt_fit(u, y, dt, y0=22.0)
        self.assertAlmostEqual(f["K_c_per_duty"], K, delta=1.5)
        self.assertAlmostEqual(f["tau_s"], tau, delta=0.1 * tau)
        self.assertAlmostEqual(f["theta_s"], theta, delta=2 * dt)
        t = analyze_capture.tune(f, 15.0)
        self.assertGreaterEqual(t["simc_tight"]["kp"], t["simc_smooth"]["kp"])
        for name in ("simc_tight", "simc_smooth"):
            self.assertGreaterEqual(t[name]["tau_c_s"], f["tau_s"] / 3)
        # tau_c >= tau/3 caps Kp at tau / (K (tau/3 + theta)) < 3 / K
        self.assertLess(t["simc_tight"]["kp"], 3 / K)
        f["tau_identifiable"] = True
        f["r2"] = 0.999
        self.assertTrue(analyze_capture.gains_usable(f))
        f["r2"] = 0.95
        self.assertFalse(analyze_capture.gains_usable(f))


# ------------------------------------------------------------------- synthetic captures

class SyntheticAnalysisTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.res = {}
        for s in synth_capture.SCENARIOS:
            d = os.path.join(cls.tmp.name, s)
            synth_capture.generate(s, d)
            cls.res[s] = (common.load_capture(d), None)
        cls.res = {s: (cap, analyze_capture.analyze(cap)) for s, (cap, _) in cls.res.items()}

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def flags(self, s):
        return " | ".join(t for _, t in self.res[s][1]["flags"])

    def test_normal(self):
        cap, r = self.res["normal"]
        self.assertEqual(r["first_heat"]["model"]["model_state"], "pass")
        self.assertEqual(r["first_heat"]["at_12s"]["rule"], "pass")
        self.assertTrue(r["first_heat"]["cold"])
        self.assertTrue(r["cold"]["agree"])
        self.assertFalse(r["cold"]["rref_hint"])
        self.assertGreater(r["airflow"]["ratio_median"], 0.08)
        self.assertEqual(r["airflow"]["foldback_cycles"], 0)
        self.assertIsNotNone(r["airflow"]["ptc_cool_tau_s_median"])
        f = r["step"]["fits"]["chamber_c"]
        self.assertTrue(20 < f["K_c_per_duty"] < 45, f)
        self.assertGreater(f["r2"], 0.8)
        self.assertLess(r["step"]["overshoot_c"], 2.0)
        self.assertTrue(r["replay"]["ok"])
        self.assertEqual(r["replay"]["unexplained"], 0)
        self.assertTrue(r["health"]["found"])
        self.assertFalse(r["health"]["flags"])
        rows = {a: (b, c) for a, b, c in analyze_capture.numbers_table(r)}
        self.assertFalse(r["step"]["fits"]["chamber_c"]["tau_identifiable"])
        self.assertEqual(rows["PID gains"][0], "not given")          # 70 min is less than 3 tau
        self.assertFalse(any(k.startswith("PID gains (SIMC") for k in rows))
        sg = r["first_heat"]["suggest"]
        self.assertGreaterEqual(sg["PB_FH_CROSS_RISE_DC"], sg["PB_FH_PASS_RISE_DC"])
        self.assertNotIn("FAIL", [lvl for lvl, _ in r["flags"]])

    def test_crossed(self):
        _, r = self.res["crossed"]
        self.assertEqual(r["first_heat"]["model"]["model_state"], "crossed")
        self.assertIn("CROSSED", self.flags("crossed"))

    def test_dead_element(self):
        _, r = self.res["dead_element"]
        self.assertEqual(r["first_heat"]["model"]["model_state"], "no_rise")
        self.assertEqual(r["first_heat"]["at_12s"]["rule"], "wait")     # the firmware has no decision yet
        self.assertEqual(r["first_heat"]["at_30s"]["rule"], "no_rise")
        self.assertLess(r["step"]["fits"]["chamber_c"]["K_c_per_duty"], 2.0)
        self.assertIn("NO_RISE", self.flags("dead_element"))

    def test_stalled_fan(self):
        _, r = self.res["stalled_fan"]
        n = self.res["normal"][1]
        self.assertLess(r["airflow"]["ratio_median"], 0.05)
        self.assertGreater(r["airflow"]["foldback_per_hour"], 6)
        self.assertGreater(r["airflow"]["ptc_max_heating_c"], n["airflow"]["ptc_max_heating_c"] + 20)
        self.assertIn("airflow", self.flags("stalled_fan"))

    def test_wrong_rref(self):
        _, r = self.res["wrong_rref"]
        c = r["cold"]
        self.assertTrue(c["agree"])                    # both channels are wrong in the same way
        self.assertTrue(c["rref_hint"])
        self.assertAlmostEqual(c["with_other_rref"]["chamber_c"], 22.0, delta=0.5)
        self.assertIn("wrong Rref", self.flags("wrong_rref"))

    def test_ptc_offset_like_issue_122(self):
        _, r = self.res["ptc_offset"]
        self.assertFalse(r["cold"]["agree"])
        self.assertAlmostEqual(r["cold"]["diff_median_c"], 24.0, delta=1.5)
        self.assertTrue(r["cold"]["oscillation"])
        self.assertIn("issue #122", self.flags("ptc_offset"))

    def test_stored_offsets_are_removed(self):
        d = os.path.join(self.tmp.name, "normal-offset")
        synth_capture.generate("normal", d, heat_s=600, cool_s=60)
        cap = common.load_capture(d)
        cap.header["calibration"] = {"chamber_offset_c": -1.0, "ptc_offset_c": 2.0}
        for r in cap.rows:                       # the state reports calibrated values
            r["chamber_c"] -= 1.0
            r["ptc_c"] += 2.0
        c = analyze_capture.section_cold(cap)
        self.assertAlmostEqual(c["diff_median_c"], 0.0, delta=0.2)      # raw agreement
        self.assertAlmostEqual(c["chamber_median_c"], 22.0, delta=0.2)
        o = c["suggest_offsets"]                 # reference 22 C in the header
        self.assertAlmostEqual(o["chamber_offset_c"], 0.0, delta=0.15)  # = -1 + (22 - 21)
        self.assertAlmostEqual(o["ptc_offset_c"], 0.0, delta=0.15)      # = +2 + (22 - 24)
        del cap.header["ambient_c"]
        o = analyze_capture.section_cold(cap)["suggest_offsets"]
        self.assertAlmostEqual(o["chamber_offset_c"], -1.0, delta=0.15)  # unchanged
        self.assertAlmostEqual(o["ptc_offset_c"], -1.0, delta=0.15)      # PTC to calibrated chamber

    def test_report_and_expect_files(self):
        d = os.path.join(self.tmp.name, "normal")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(analyze_capture.main([d, "--quiet", "--write-expect", "--mains-v", "230",
                                                   "--rated-v", "230"]), 0)
        with open(os.path.join(d, "analysis.md")) as f:
            md = f.read()
        for part in ("## Priors", "## First heat (measured)", "## Cold agreement", "## Airflow",
                     "## Step response", "## Control health", "## Replay", "PID gains"):
            self.assertIn(part, md)
        self.assertLess(md.index("## Priors"), md.index("## First heat (measured)"))
        with open(os.path.join(d, "analysis.json")) as f:
            json.load(f)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(replay_capture.main([d, "--expect", os.path.join(d, "expect.json")]), 0)
        with open(os.path.join(d, "expect.json")) as f:
            self.assertIn("first_heat.model.model_state", json.load(f)["checks"])

    def test_first_heat_columns_and_firmware_agreement(self):
        d = os.path.join(self.tmp.name, "fh-crossed")
        synth_capture.generate("crossed", d, heat_s=600, cool_s=300, fh_columns=True)
        cap = common.load_capture(d)
        rp = replay_capture.replay(cap)
        self.assertEqual(rp["first_heat"]["firmware_state"], "crossed")
        self.assertEqual(rp["first_heat"]["model_state"], "crossed")
        self.assertTrue(rp["ok"])
        self.assertTrue(any(r["fault"] for r in cap.rows))


class RecordedCaptureTest(unittest.TestCase):
    """Each tests/captures/NAME/ with an expect.json is a regression test (docs/CAPTURES.md)."""

    def test_recorded_captures(self):
        base = ROOT / "tests" / "captures"
        dirs = sorted(p for p in base.glob("*/") if (p / "expect.json").exists()) if base.exists() else []
        if not dirs:
            self.skipTest("no recorded captures yet")
        for d in dirs:
            with self.subTest(capture=d.name):
                cap = common.load_capture(str(d))
                with open(d / "expect.json") as f:
                    expect = json.load(f)
                self.assertTrue(replay_capture.replay(cap)["ok"])
                self.assertEqual(replay_capture.check_expect(analyze_capture.analyze(cap), expect), [])


class ReplayTest(unittest.TestCase):
    def test_violation_is_found(self):
        with tempfile.TemporaryDirectory() as d:
            synth_capture.generate("normal", d, idle_s=60, heat_s=300, cool_s=60)
            cap = common.load_capture(d)
            hot = 0
            for r in cap.rows:
                if r["relay"] == 1 and hot < 3:
                    r["ptc_c"] = 106.0
                    hot += 1
            res = replay_capture.replay(cap)
            self.assertFalse(res["ok"])
            self.assertEqual(res["violations"][0]["rule"], "relay on with PTC >= 105 C")

    def test_export_adc(self):
        with tempfile.TemporaryDirectory() as d:
            synth_capture.generate("normal", d, idle_s=30, heat_s=60, cool_s=10)
            cap = common.load_capture(d)
            out = os.path.join(d, "adc.csv")
            n = replay_capture.export_adc(cap, out)
            self.assertEqual(n, 201)
            with open(out) as f:
                lines = f.read().splitlines()
            t_ms, ch_mv, ptc_mv, *_ = lines[1].split(",")
            self.assertAlmostEqual(ntc.mv_to_temp_c(float(ch_mv), 82), cap.rows[0]["chamber_c"], delta=0.1)

    def test_diag_csv_is_accepted(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "dragonbreath_diag.csv")
            with open(p, "w") as f:
                f.write("t_s,chamber_c,ptc_c,ptc_status,out,mode,fault,reason,peak_c\n")
                for i in range(200):
                    on = 1 if i >= 100 else 0
                    ptc = 22 + (0.8 * (i - 100) if on else 0)
                    f.write(f"{i * 0.5:.1f},22.0,{ptc:.1f},ok,{on},power_on,0,,{ptc:.1f}\n")
            cap = common.load_capture(p)
            self.assertEqual(cap.kind, "diag")
            r = analyze_capture.analyze(cap)
            self.assertEqual(r["first_heat"]["at_12s"]["rule"], "pass")
            self.assertFalse(r["replay"]["modeled"])
            with contextlib.redirect_stdout(io.StringIO()):
                analyze_capture.main([p, "--quiet"])
            self.assertTrue(os.path.exists(p + ".analysis.md"))


# ------------------------------------------------------------------- capture tool + mock device

TOKEN = "tok"


class MockDevice:
    """A small API v2 device: a thermal model that runs SPEED times faster than real time."""
    SPEED = 20.0

    def __init__(self, metrics=True):
        self.lock = threading.Lock()
        self.metrics = metrics
        self.rev = 1
        self.mode, self.target, self.output = "off", 0.0, False
        self.fault, self.reason = False, None
        self.lease = None
        self.chamber, self.ptc = 22.0, 22.0
        self.last = time.monotonic()
        self.requests = []
        self.fan = 0
        self.comms_ms = 300000
        self.beat_at = None
        self.beats = 0
        self.metrics_delay = 0.0

    def advance(self):
        now = time.monotonic()
        if self.lease and now - self.beat_at > self.comms_ms / 1000.0:
            self.fault, self.reason = True, "Controller lease expired"
            self.mode, self.lease = "off", None
        dt = (now - self.last) * self.SPEED
        self.last = now
        if self.mode == "power_on" and not self.fault:
            if self.chamber < self.target - 1:
                self.output = True
            elif self.chamber >= self.target:
                self.output = False
        else:
            self.output = False
        self.fan = 100 if self.mode == "power_on" or self.ptc > 40 else 0
        self.ptc += dt * ((2.0 if self.output else 0.0) - 0.08 * (self.ptc - self.chamber))
        self.chamber += dt * (0.01 * (self.ptc - self.chamber) - 0.002 * (self.chamber - 22.0))

    def state(self):
        return {
            "api_version": 2, "device_id": "mock", "boot_id": "b" * 32, "firmware": "9.9.9-mock",
            "state_revision": self.rev, "mode": self.mode, "source": "web",
            "target": {"requested_c": self.target, "effective_c": self.target, "maximum_c": 70.0},
            "config": {"max": 70.0, "max_abs": 70.0, "comms_ms": self.comms_ms},
            "heater": {"demand": self.mode == "power_on", "output": self.output, "method": "bangbang"},
            "fan": {"requested_percent": self.fan, "effective_percent": self.fan, "reason": "heater"},
            "sensors": {"chamber": {"temperature_c": round(self.chamber, 1), "status": "ok"},
                        "ptc": {"temperature_c": round(self.ptc, 1), "status": "ok"}},
            "control": {"lease": {"active": self.lease is not None,
                                  "owner": "capture-tool" if self.lease else None}},
            "safety": {"fault_latched": self.fault, "inhibited": False, "reason": self.reason},
        }


def make_handler(dev):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, code, obj, ctype="application/json"):
            body = obj.encode() if isinstance(obj, str) else json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            with dev.lock:
                dev.requests.append(("GET", self.path))
                dev.advance()
                if self.path == "/api/v2/info":
                    return self.send(200, {"api_version": 2, "device_id": "mock", "firmware": "9.9.9-mock",
                                           "rref_kohm": 82})
                if self.path == "/api/v2/state":
                    return self.send(200, dev.state())
                if self.path == "/settings":
                    return self.send(200, {"max": 70.0, "fb_cut": 0, "fb_cut_default": 102})
                if self.path == "/api/v2/calibration":
                    return self.send(200, {"api_version": 2, "chamber_offset_c": 0, "ptc_offset_c": 0})
                if self.path == "/api/v2/heater_method":
                    return self.send(200, {"api_version": 2, "method": "bangbang"})
                if self.path == "/api/v2/health":
                    return self.send(200, {"uptime_s": 100})
                if self.path == "/metrics" and dev.metrics:
                    if dev.metrics_delay:
                        dev.lock.release()
                        try:
                            time.sleep(dev.metrics_delay)
                        finally:
                            dev.lock.acquire()
                    return self.send(200, "dragonbreath_uptime_seconds 100\n"
                                          "dragonbreath_control_ticks_late_total 0\n", "text/plain")
                return self.send(404, {"error": "not_found"})

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}")
            with dev.lock:
                dev.requests.append(("POST", self.path, body))
                dev.advance()
                if self.headers.get("X-DragonBreath-Auth") != TOKEN:
                    return self.send(403, {"ok": False, "error": "auth_failed"})
                if self.path == "/api/v2/command":
                    name = body["command"]["name"]
                    if name == "off":
                        dev.mode, dev.target, dev.output, dev.lease = "off", 0.0, False, None
                        dev.rev += 1
                        return self.send(200, {"ok": True, "state": dev.state()})
                    if name == "power_on":
                        if body.get("expected_revision") != dev.rev:
                            return self.send(409, {"ok": False, "error": "revision_conflict"})
                        dev.mode, dev.target, dev.lease = "power_on", body["command"]["target_c"], "L" * 32
                        dev.beat_at = time.monotonic()
                        dev.rev += 1
                        dev.advance()
                        return self.send(200, {"ok": True, "lease_id": dev.lease, "state": dev.state()})
                    return self.send(400, {"ok": False, "error": "invalid_command"})
                if self.path == "/api/v2/heartbeat":
                    ok = body.get("lease_id") == dev.lease and dev.lease is not None
                    if ok:
                        dev.beat_at, dev.beats = time.monotonic(), dev.beats + 1
                    return self.send(200 if ok else 409, {"ok": ok} if ok else {"ok": False,
                                                                                "error": "stale_lease"})
                return self.send(404, {"error": "not_found"})
    return H


class CaptureToolTest(unittest.TestCase):
    def setUp(self):
        self.dev = MockDevice()
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(self.dev))
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.host = f"http://127.0.0.1:{self.srv.server_address[1]}"
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        self.tmp.cleanup()

    def run_tool(self, *args, ask=lambda _: "y"):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            rc = capture.main([self.host, *args, "--out", self.tmp.name], ask=ask)
        dirs = sorted(os.listdir(self.tmp.name))
        return rc, out.getvalue(), (os.path.join(self.tmp.name, dirs[-1]) if dirs else None)

    def posts(self):
        return [r for r in self.dev.requests if r[0] == "POST"]

    def test_idle_is_read_only_and_writes_a_capture(self):
        rc, out, d = self.run_tool("idle", "--duration", "1.5", "--rate", "10", "--metrics-every", "0.5",
                                   "--notes", "bench", "--ambient-c", "21.5")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.posts(), [])
        cap = common.load_capture(d)
        self.assertGreater(len(cap.rows), 8)
        self.assertGreaterEqual(len(cap.metrics), 2)
        self.assertTrue(cap.header["complete"])
        self.assertEqual(cap.header["notes"], "bench")
        self.assertEqual(cap.header["rref_kohm"], 82)
        self.assertEqual(cap.header["heater_method"], "bangbang")
        self.assertNotIn("token", cap.header["params"])
        self.assertTrue(os.path.exists(os.path.join(d, "state.jsonl.gz")))

    def test_metrics_404_is_a_note(self):
        self.dev.metrics = False
        rc, out, d = self.run_tool("soak", "--duration", "1", "--rate", "5", "--metrics-every", "0.2")
        self.assertEqual(rc, 0, out)
        cap = common.load_capture(d)
        self.assertEqual(cap.metrics, [])
        self.assertIn("no /metrics", " ".join(e[2] for e in cap.events))

    def test_control_needs_explicit_token(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as e:
            capture.parse_args([self.host, "step", "--allow-control"])
        self.assertEqual(e.exception.code, 2)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            capture.parse_args([self.host, "step", "--token", TOKEN])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            capture.parse_args([self.host, "step", "--target", "65", "--allow-control", "--token", TOKEN])

    def test_step_defaults_cover_three_time_constants(self):
        a = capture.parse_args([self.host, "step"])
        self.assertGreaterEqual(a.duration, 2.5 * 3600)

    def test_first_heat_with_control_sends_only_documented_commands(self):
        rc, out, d = self.run_tool("first-heat", "--allow-control", "--token", TOKEN, "--rate", "10",
                                   "--baseline", "0.5", "--heat-seconds", "1", "--tail", "0.5",
                                   "--metrics-every", "0.5")
        self.assertEqual(rc, 0, out)
        posts = self.posts()
        self.assertTrue(all(p[1] in ("/api/v2/command", "/api/v2/heartbeat") for p in posts))
        names = [p[2]["command"]["name"] for p in posts if p[1] == "/api/v2/command"]
        self.assertEqual(names[0], "power_on")
        self.assertEqual(names[-1], "off")
        self.assertEqual(set(names), {"power_on", "off"})
        self.assertEqual(self.dev.mode, "off")
        self.assertFalse(self.dev.output)
        cap = common.load_capture(d)
        self.assertTrue(any(r["relay"] == 1 for r in cap.rows))
        self.assertEqual(cap.rows[-1]["relay"], 0)

    def test_first_heat_read_only_follows_the_operator(self):
        def operator():
            time.sleep(0.6)
            with self.dev.lock:               # the operator presses On
                self.dev.mode, self.dev.target = "power_on", 40.0
            time.sleep(1.2)
            with self.dev.lock:               # the operator presses Power
                self.dev.mode, self.dev.target = "off", 0.0
        threading.Thread(target=operator, daemon=True).start()
        rc, out, d = self.run_tool("first-heat", "--rate", "10", "--baseline", "0.3",
                                   "--heat-seconds", "0.5", "--tail", "0.3", "--wait", "5")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.posts(), [])
        self.assertIn("press On", out)
        self.assertIn("stop the heat now", out)
        cap = common.load_capture(d)
        self.assertTrue(any(r["relay"] == 1 for r in cap.rows))
        self.assertEqual(cap.header["stop_reason"], "first heat recorded")

    def test_declined_power_on_sends_nothing(self):
        rc, out, d = self.run_tool("step", "--allow-control", "--token", TOKEN, "--rate", "10",
                                   ask=lambda _: "n")
        self.assertEqual(rc, 1)
        self.assertEqual(self.posts(), [])
        self.assertIn("declined", common.load_capture(d).header["stop_reason"])

    def test_interrupt_while_heating_sends_off(self):
        calls = {"n": 0}
        real = capture.Capturer.sample

        def sample(self_):
            calls["n"] += 1
            if calls["n"] == 15:
                raise KeyboardInterrupt
            return real(self_)
        capture.Capturer.sample = sample
        try:
            rc, out, d = self.run_tool("step", "--allow-control", "--token", TOKEN, "--rate", "10",
                                       "--yes")
        finally:
            capture.Capturer.sample = real
        self.assertEqual(rc, 1, out)                  # an interrupted capture is not a success
        names = [p[2]["command"]["name"] for p in self.posts() if p[1] == "/api/v2/command"]
        self.assertEqual(names, ["power_on", "off"])
        self.assertEqual(self.dev.mode, "off")
        self.assertEqual(common.load_capture(d).header["stop_reason"], "interrupted")

    def test_interrupt_inside_off_still_sends_off(self):
        """A second Ctrl-C (or SIGTERM) during the OFF must not stop the OFF."""
        real_cmd = capture.Client.command
        real_sample = capture.Capturer.sample
        state = {"n": 0, "off_calls": 0}

        def command(self_, name, revision=None, **args):
            if name == "off":
                state["off_calls"] += 1
                if state["off_calls"] == 1:
                    raise KeyboardInterrupt        # the second Ctrl-C, inside the first attempt
            return real_cmd(self_, name, revision, **args)

        def sample(self_):
            state["n"] += 1
            if state["n"] == 15:
                raise KeyboardInterrupt            # the first Ctrl-C
            return real_sample(self_)
        capture.Client.command, capture.Capturer.sample = command, sample
        try:
            rc, out, d = self.run_tool("step", "--allow-control", "--token", TOKEN, "--rate", "10", "--yes")
        finally:
            capture.Client.command, capture.Capturer.sample = real_cmd, real_sample
        self.assertEqual(rc, 1, out)
        self.assertGreaterEqual(state["off_calls"], 2)
        names = [p[2]["command"]["name"] for p in self.posts() if p[1] == "/api/v2/command"]
        self.assertEqual(names, ["power_on", "off"])
        self.assertEqual(self.dev.mode, "off")
        self.assertFalse(self.dev.output)

    def test_signals_are_ignored_during_off(self):
        import signal as sig
        seen = {}
        real_cmd = capture.Client.command

        def command(self_, name, revision=None, **args):
            if name == "off":
                seen["int"] = sig.getsignal(sig.SIGINT)
                seen["term"] = sig.getsignal(sig.SIGTERM)
            return real_cmd(self_, name, revision, **args)
        capture.Client.command = command
        try:
            rc, out, d = self.run_tool("first-heat", "--allow-control", "--token", TOKEN, "--rate", "10",
                                       "--baseline", "0.3", "--heat-seconds", "0.3", "--tail", "0.2")
        finally:
            capture.Client.command = real_cmd
        self.assertEqual(rc, 0, out)
        self.assertEqual(seen["int"], sig.SIG_IGN)
        self.assertEqual(seen["term"], sig.SIG_IGN)
        self.assertNotEqual(sig.getsignal(sig.SIGINT), sig.SIG_IGN)

    def test_heartbeat_is_not_delayed_by_slow_gets(self):
        self.dev.comms_ms = 2000                     # lease TTL 2 s, heartbeat each 0.5 s
        self.dev.metrics_delay = 1.2                 # each /metrics GET blocks the main loop 1.2 s
        rc, out, d = self.run_tool("first-heat", "--allow-control", "--token", TOKEN, "--rate", "10",
                                   "--metrics-every", "0.2", "--baseline", "0.2", "--heat-seconds", "4",
                                   "--tail", "0.2")
        self.assertEqual(rc, 0, out)
        self.assertFalse(self.dev.fault, self.dev.reason)
        self.assertGreaterEqual(self.dev.beats, 5)

    def test_cooldown_never_stops_foreign_heat(self):
        with self.dev.lock:
            self.dev.mode, self.dev.target = "power_on", 40.0     # a print in AUTO, or a button
        def operator():
            time.sleep(1.0)
            with self.dev.lock:
                self.dev.mode, self.dev.target = "off", 0.0
        threading.Thread(target=operator, daemon=True).start()
        rc, out, d = self.run_tool("cooldown", "--allow-control", "--token", TOKEN, "--rate", "10",
                                   "--duration", "1.5")
        self.assertEqual(self.posts(), [])
        self.assertIn("stop the heat now", out)

    def test_mains_mismatch_refuses_heat(self):
        rc, out, d = self.run_tool("step", "--allow-control", "--token", TOKEN, "--yes", "--rate", "10",
                                   "--mains-v", "120", "--rated-v", "230")
        self.assertEqual(rc, 1)
        self.assertEqual(self.posts(), [])
        self.assertIn("does not agree", common.load_capture(d).header["stop_reason"])

    def test_token_file(self):
        p = os.path.join(self.tmp.name, "..", f"tok-{os.getpid()}")
        with open(p, "w") as f:
            f.write(TOKEN + "\n")
        try:
            a = capture.parse_args([self.host, "step", "--allow-control", "--token-file", p])
        finally:
            os.unlink(p)
        self.assertEqual(a.token, TOKEN)

    def test_fault_latched_refuses_control(self):
        self.dev.fault, self.dev.reason = True, "PTC element over-temp"
        rc, out, d = self.run_tool("step", "--allow-control", "--token", TOKEN, "--yes", "--rate", "10")
        self.assertEqual(rc, 1)
        self.assertEqual(self.posts(), [])
        self.assertIn("never clears", common.load_capture(d).header["stop_reason"])


if __name__ == "__main__":
    unittest.main()
