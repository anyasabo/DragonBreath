#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write SYNTHETIC DragonBreath captures from a simple thermal model.

The data is not real. It exists to test tools/analyze_capture.py and
tools/replay_capture.py before the hardware is here, and to show what each fault
looks like in a capture.

Plant (lumped, one node each):
  element   C_el dT/dt = P(T_el) * relay - h * (T_el - T_air)    PTC self-limit in P()
  air       C_air dT/dt = h * (T_el - T_air) - G * (T_air - T_amb)
  sensors   PTC NTC on the element, chamber NTC in the outlet stream:
            outlet = T_air + eff * (T_el - T_air), both with a first-order lag
  h, eff    depend on the fan (on: forced airflow, off: natural convection)
The controller is tools/heater_model.py (bang-bang, foldback, cutoffs) plus a
fan rule (on while armed, purge after heat until both sensors are below 40 C).

Scenarios: normal, crossed, dead_element, stalled_fan, wrong_rref, ptc_offset.

  python3 tools/synth_capture.py normal /tmp/synth-normal
"""
import argparse
import datetime
import json
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dbcapture_common as common  # noqa: E402
import heater_model as hm  # noqa: E402
import ntc_model as ntc  # noqa: E402

SCENARIOS = ("normal", "crossed", "dead_element", "stalled_fan", "wrong_rref", "ptc_offset")
TICK_S = 0.5


class Plant:
    def __init__(self, scenario, ambient_c):
        self.amb = ambient_c
        self.t_el = self.t_air = ambient_c
        self.s_ptc = self.s_out = ambient_c
        self.p0 = 0.0 if scenario == "dead_element" else 300.0
        self.c_el = 120.0             # J/K, element + fins
        self.c_air = 28000.0          # J/K, enclosure (tau = c_air / g = 47 min)
        self.g = 10.0                 # W/K, enclosure loss
        self.h_on, self.h_off = 12.0, 1.0
        self.eff_on, self.eff_off = 0.15, 0.03
        if scenario == "stalled_fan":          # blower turns very slowly
            self.h_on, self.eff_on = 3.0, 0.04
        self.tau_ptc, self.tau_out = 6.0, 15.0

    def power(self):
        return self.p0 / (1.0 + math.exp((self.t_el - 125.0) / 5.0))

    def step(self, relay, fan, dt):
        h = self.h_on if fan else self.h_off
        eff = self.eff_on if fan else self.eff_off
        q = h * (self.t_el - self.t_air)
        self.t_el += dt * ((self.power() if relay else 0.0) - q) / self.c_el
        self.t_air += dt * (q - self.g * (self.t_air - self.amb)) / self.c_air
        out = self.t_air + eff * (self.t_el - self.t_air)
        self.s_ptc += dt * (self.t_el - self.s_ptc) / self.tau_ptc
        self.s_out += dt * (out - self.s_out) / self.tau_out


def _metrics_text(t_s, ticks, late, relay_s, boots=1):
    heap = 98000 - int(t_s) % 700
    lines = [
        'dragonbreath_build_info{version="synthetic",idf_version="v5.3.6"} 1',
        f"dragonbreath_uptime_seconds {t_s + 30:.0f}",
        f"dragonbreath_boots_total {boots}",
        'dragonbreath_reset_reason_info{reason="poweron"} 1',
        f"dragonbreath_control_ticks_total {ticks}",
        f"dragonbreath_control_ticks_late_total {late}",
        "dragonbreath_control_tick_duration_seconds 0.000412",
        "dragonbreath_control_tick_duration_max_seconds 0.001930",
        "dragonbreath_control_tick_interval_max_seconds 0.512000",
        f"dragonbreath_heap_free_bytes {heap}",
        "dragonbreath_heap_minimum_free_bytes 81234",
        "dragonbreath_heap_largest_free_block_bytes 61440",
        f"dragonbreath_relay_on_seconds_total {relay_s:.1f}",
        'dragonbreath_task_stack_free_minimum_bytes{task="control"} 1880',
        'dragonbreath_task_stack_free_minimum_bytes{task="httpd"} 1312',
        'dragonbreath_task_stack_free_minimum_bytes{task="IDLE"} 640',
    ]
    return "\n".join(lines) + "\n"


def generate(scenario, out_dir, ambient_c=22.0, idle_s=900.0, heat_s=4200.0, cool_s=1200.0,
             target_c=40.0, sample_s=1.0, seed=1, fh_columns=False):
    """Write one synthetic session: idle, heat at target_c, then off with the purge fan."""
    if scenario not in SCENARIOS:
        raise ValueError(f"unknown scenario {scenario}")
    rnd = random.Random(seed)
    true_rref = 82
    fw_rref = 33 if scenario == "wrong_rref" else 82
    plant = Plant(scenario, ambient_c)
    ctl = hm.BangBangModel(rref_kohm=fw_rref)
    fh = hm.FirstHeatModel()
    header = {
        "scenario": "synthetic", "synthetic": scenario, "notes": f"synthetic {scenario}",
        "ambient_c": ambient_c, "host": "synthetic",
        "started_utc": datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc).isoformat(),
        "info": {"firmware": "synthetic", "rref_kohm": fw_rref, "device_id": "synthetic"},
        "settings": {"fb_cut": 0, "fb_cut_default": hm.foldback_thresholds(fw_rref)[0],
                     "cool_release": hm.COOL_RELEASE_C},
        "heater_method": "bangbang",
        "params": {"target_c": target_c, "idle_s": idle_s, "heat_s": heat_s, "cool_s": cool_s},
    }
    w = common.CaptureWriter(out_dir, header)
    win = {"ptc": [], "ch": []}
    relay = fan = False
    session_heat = purge = False
    total = idle_s + heat_s + cool_s
    n = int(round(total / TICK_S))
    every = max(1, int(round(sample_s / TICK_S)))
    ticks, relay_s, rev, mode, fault_reason = 0, 0.0, 1, "off", None
    w.event(0.0, "phase", "idle")
    for i in range(n + 1):
        t = i * TICK_S
        heating_phase = idle_s <= t < idle_s + heat_s
        new_mode = "power_on" if heating_phase and not ctl.latched else "off"
        if new_mode != mode:
            rev += 1
            mode = new_mode
            w.event(t, "phase", "heat" if mode == "power_on" else "off")
        # true -> sensor -> firmware reading
        ptc_true, ch_true = plant.s_ptc, plant.s_out
        if scenario == "crossed":
            ptc_true, ch_true = ch_true, ptc_true
        readings = []
        for v in (ptc_true, ch_true):
            if scenario == "wrong_rref":
                v = ntc.reinterpret_c(v, true_rref, fw_rref)   # true value -> displayed value
            readings.append(v)
        ptc_r, ch_r = readings
        if scenario == "ptc_offset":
            ptc_r = ptc_r + 24.0 + 2.5 * math.sin(t / 40.0)
        ptc_r = round(max(ntc.T_MIN_C, min(ntc.T_MAX_C, ptc_r + rnd.gauss(0, 0.05))), 1)
        ch_r = round(max(ntc.T_MIN_C, min(ntc.T_MAX_C, ch_r + rnd.gauss(0, 0.05))), 1)
        target = target_c if mode == "power_on" else 0.0
        relay, constraint = ctl.step(ch_r, ptc_r, target)
        if ctl.latched and fault_reason is None:
            fault_reason = ctl.trip
            rev += 1
        if fh_columns:
            fh.feed(t, int(relay), ptc_r, ch_r)
            if fh.state in ("crossed", "no_rise") and not ctl.latched:
                ctl.latched, ctl.trip, fault_reason = True, "first_heat_" + fh.state, fh.state
                relay, rev = False, rev + 1
        if relay:
            session_heat = True
            relay_s += TICK_S
        hot = max(ptc_r, ch_r)
        if session_heat and hot >= hm.COOL_RELEASE_C + hm.PURGE_BAND_C:
            purge = True
        elif ptc_r < hm.COOL_RELEASE_C and ch_r < hm.COOL_RELEASE_C:
            purge = False
        armed = target > 0 and not ctl.latched
        fan = armed or purge or ctl.latched
        fan_reason = ("fault" if ctl.latched else "heater" if armed else
                      "thermal_purge" if purge else "off")
        win["ptc"] = (win["ptc"] + [ptc_r])[-5:]
        win["ch"] = (win["ch"] + [ch_r])[-5:]
        if i % every == 0:
            st = {
                "api_version": 2, "boot_id": "synthetic", "state_revision": rev, "mode": mode,
                "source": "web", "firmware": "synthetic",
                "target": {"effective_c": target},
                "heater": {"demand": armed, "output": relay, "constraint": constraint,
                           "method": "bangbang", "commanded_duty": 1.0 if relay else 0.0},
                "fan": {"effective_percent": 100 if fan else 0, "reason": fan_reason},
                "sensors": {"chamber": {"temperature_c": round(sum(win["ch"]) / len(win["ch"]), 1),
                                        "status": "ok"},
                            "ptc": {"temperature_c": round(sum(win["ptc"]) / len(win["ptc"]), 1),
                                    "status": "ok"}},
                "safety": {"fault_latched": ctl.latched, "inhibited": False,
                           "reason": fault_reason},
            }
            if fh_columns:
                st["first_heat"] = {"state": fh.state, "ptc_rise_c": fh.ptc_rise_c,
                                    "ch_rise_c": fh.ch_rise_c, "stock_hint": "none"}
            w.state(common.flatten_state(st, t, 1767225600.0 + t), st)
        if i % int(15 / TICK_S) == 0:
            w.metrics(t, _metrics_text(t, ticks, 0, relay_s))
        plant.step(relay, fan, TICK_S)
        ticks += 1
    w.close(ended_utc=None, stop_reason="synthetic end")
    return out_dir


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    p.add_argument("scenario", choices=SCENARIOS)
    p.add_argument("out_dir")
    p.add_argument("--ambient-c", type=float, default=22.0)
    p.add_argument("--target", type=float, default=40.0)
    p.add_argument("--first-heat-columns", action="store_true",
                   help="add the first_heat fields of feat/first-heat-check (and its latch)")
    a = p.parse_args(argv)
    generate(a.scenario, a.out_dir, ambient_c=a.ambient_c, target_c=a.target,
             fh_columns=a.first_heat_columns)
    with open(os.path.join(a.out_dir, "header.json")) as f:
        print(json.dumps({k: v for k, v in json.load(f).items() if k in ("synthetic", "samples")}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
