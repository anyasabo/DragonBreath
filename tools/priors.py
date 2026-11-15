#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Expected values (priors) for a DragonBreath capture, from physics and known data.

A capture is then a check against a prediction, not a discovery. All numbers here
are ESTIMATES with stated assumptions. The measured values in a capture win.

Parts:
  forensics     the reading of each plausible thermistor-chain mistake at a known
                ambient (pb_ntc divider formula and R/T table, tools/ntc_model.py)
  inverse       from an idle reading pair at a shared ambient: the resistance ratio
                and the mistakes that fit it (hypotheses, not a diagnosis)
  first heat    initial element rise from 300 W into the element mass
  outlet        outlet air rise from the blower airflow
  mains         PTC power and fan at 120 V or 230 V mains (an input, never a guess)
  enclosure     CORE One L enclosure: time constant and loss from the printer's sensor

Usage:
  python3 tools/priors.py --ambient-c 22
  python3 tools/priors.py --idle 25.5 49.6 --rref 82       # upstream issue #122
  python3 tools/priors.py --mains-v 120 --rated-v 220
Standard library only.
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ntc_model as ntc  # noqa: E402

POWER_W = 300.0                       # nominal PTC power (docs/HARDWARE.md)
ALU_C_J_PER_G_K = 0.9
ELEMENT_G = (100.0, 200.0)            # element + fins, aluminium (assumption)
AIRFLOW_M3_MIN = (0.1, 0.2)           # blower, not measured (assumption)
AIR_RHO = 1.2
AIR_CP = 1005.0
FH_PASS_C, FH_WINDOW_S = 3.0, 12.0    # provisional first-heat rule (feat/first-heat-check)
FOLDBACK_CUT_C = 102.0                # 82k board
FAN_RATED_V = 220.0                   # STKJ ST7530B220H EC blower (docs/HARDWARE.md)

# Upstream issue #122 (v1.1.20, USB-only power, idle, firmware reported Rref 82 k).
ISSUE_122_IDLE = {"chamber_c": (25.4, 25.7), "ptc_c": (49.5, 49.8), "rref_kohm": 82}
# Issue #122 under heat: PTC 63.5 -> 98.4 C in about 24 s (13:31:44.8 -> 13:32:09.0).
ISSUE_122_HEAT = {"ptc_from_c": 63.5, "ptc_to_c": 98.4, "seconds": 24.2}

# CORE One L enclosure, 30 days of the printer's own chamber sensor, no chamber heater.
ENCLOSURE = {
    "tau_min_median": 47.0, "tau_min_range": (27.0, 83.0), "cooldowns": 12,
    "room_c": (19.0, 24.0),
    "bed_rise_c": {60: (10.0, 12.0), 85: (13.0, 14.0)}, "bed115_chamber_c": (54.0, 55.0),
    "loss_w_per_k": (8.0, 12.0),     # ESTIMATE: assumed 100-150 W of bed heat at bed 85 C
    "bed_w_into_chamber": (100.0, 150.0),
}


def display_c(true_c, rref_used, rref_true=None, k=1.0, leak_kohm=None):
    """Displayed temperature for a true temperature and a chain mistake."""
    r = ntc.temp_to_r_kohm(true_c) * k
    if leak_kohm:
        r = r * leak_kohm / (r + leak_kohm)
    if rref_true:
        r = r * rref_used / rref_true
    return ntc.r_to_temp_c(r)


def forensics(ambient_c, rref_used=82):
    other = 33 if rref_used == 82 else 82
    rows = [("correct chain", display_c(ambient_c, rref_used), "both channels")]
    rows.append((f"wrong Rref: firmware {rref_used} k, divider {other} k",
                 display_c(ambient_c, rref_used, other), "both channels (one strap)"))
    rows.append((f"wrong Rref: firmware {other} k, divider {rref_used} k",
                 display_c(ambient_c, other, rref_used), "both channels (one strap)"))
    for k, label in ((0.1, "10 k NTC (R25 x0.1)"), (0.37, "R25 x0.37"), (0.5, "R25 x0.5"),
                     (2.0, "R25 x2"), (3.0, "R25 x3")):
        rows.append((f"thermistor {label}", display_c(ambient_c, rref_used, k=k), "one channel"))
    for leak in (47.0, 100.0, 220.0):
        rows.append((f"parallel leak {leak:.0f} k", display_c(ambient_c, rref_used, leak_kohm=leak),
                     "one channel"))
    rows.append(("swapped channels", ambient_c, "no idle sign: both see ambient. Shows at first heat."))
    rows.append(("open thermistor", None, "status open, no temperature"))
    rows.append(("shorted thermistor", None, "status short, no temperature"))
    return rows


def inverse(chamber_c, ptc_c, ambient_c=None, rref_used=82, tol=0.10):
    """Resistance ratio R_expected/R_displayed per channel and the mistakes that fit.

    ratio > 1: the display is hotter than the ambient (lower resistance than expected).
    If ambient_c is None, the chamber reading is the reference for the PTC channel.
    """
    ref = chamber_c if ambient_c is None else ambient_c
    out = {"reference_c": ref, "reference": "chamber channel" if ambient_c is None else "ambient",
           "channels": {}}
    for name, read in (("chamber", chamber_c), ("ptc", ptc_c)):
        if read is None:
            continue
        if ntc.is_clamped(read):
            out["channels"][name] = {"reading_c": read, "ratio": None,
                                     "fits": ["reading at a table end: the resistance is outside "
                                              "the table (open, short, or a large Rref error)"]}
            continue
        ratio = ntc.temp_to_r_kohm(ref) / ntc.temp_to_r_kohm(read)
        fits = []
        if abs(ratio - 1) <= tol:
            fits.append("no chain error (within 10 %)")
        else:
            implied = rref_used * ratio
            for rr in ntc.RREF_CHOICES:
                if rr != rref_used and abs(implied / rr - 1) <= tol:
                    fits.append(f"wrong Rref: divider {rr} k, firmware uses {rref_used} k")
            mirror = 33 if rref_used == 82 else 82
            alt = display_c(ref, mirror, rref_used)
            if abs(ratio - rref_used / mirror) / (rref_used / mirror) <= 0.15 and not fits:
                fits.append(f"near the mirror Rref error (firmware {mirror} k on a {rref_used} k "
                            f"divider gives {alt:.1f} C), but the device reported {rref_used} k: "
                            "only if the strap report is wrong, and not exact")
            fits.append(f"thermistor R25 {1 / ratio:.2f} x the table part")
            if ratio > 1:
                r_read = ntc.temp_to_r_kohm(read)
                r_true = ntc.temp_to_r_kohm(ref)
                fits.append(f"parallel leak of about {r_read * r_true / (r_true - r_read):.0f} k "
                            "(contamination, damaged cable, a wrong part)")
            else:
                fits.append("series resistance (bad crimp, corroded contact)")
            fits.append("electrical noise or a supply effect on the ADC (compare USB and mains)")
        out["channels"][name] = {"reading_c": read, "ratio": ratio,
                                 "implied_rref_kohm": rref_used * ratio, "fits": fits}
    return out


def first_heat(power_w=POWER_W, element_g=ELEMENT_G):
    rows = []
    for m in element_g:
        rate = power_w / (m * ALU_C_J_PER_G_K)
        rise = rate * FH_WINDOW_S
        rows.append({"element_g": m, "rate_c_s": rate, "rise_12s_c": rise,
                     "margin_x": rise / FH_PASS_C})
    h = ISSUE_122_HEAT
    rate122 = (h["ptc_to_c"] - h["ptc_from_c"]) / h["seconds"]
    return {"rows": rows, "issue122_rate_c_s": rate122,
            "issue122_effective_g": power_w / (rate122 * ALU_C_J_PER_G_K)}


def outlet(power_w=POWER_W, airflow=AIRFLOW_M3_MIN, inlet_c=25.0):
    rows = []
    for q in airflow:
        mdot = q / 60.0 * AIR_RHO
        dt = power_w / (mdot * AIR_CP)
        cap = FOLDBACK_CUT_C - inlet_c
        rows.append({"airflow_m3_min": q, "rise_c": dt, "capped_c": min(dt, cap),
                     "capped": dt > cap})
    return rows


def mains(mains_v, rated_v, power_w=POWER_W):
    """PTC power scales about with V^2 (cold resistance). The self-limit makes the real
    drop smaller when the element is hot. Fan: a 220 VAC EC blower."""
    p = power_w * (mains_v / rated_v) ** 2
    fan = "rated voltage" if abs(mains_v - FAN_RATED_V) / FAN_RATED_V < 0.15 else (
        "far below the fan rating: the blower can run slow or not start; then the element "
        "reaches foldback fast and the outlet rise is small")
    return {"mains_v": mains_v, "rated_v": rated_v, "ptc_power_w": p, "fan": fan}


def enclosure(power_w=POWER_W, start_c=22.0, target_c=50.0):
    e = ENCLOSURE
    g_lo, g_hi = e["loss_w_per_k"]
    tau = e["tau_min_median"]
    rows = []
    for bed_w in (0.0,) + e["bed_w_into_chamber"]:
        for g in (g_lo, g_hi):
            dss = (power_w + bed_w) / g
            need = target_c - start_c
            t = -tau * math.log(1 - need / dss) if dss > need else None
            rows.append({"bed_w": bed_w, "loss_w_k": g, "steady_rise_c": dss, "minutes": t})
    return {"tau_min": tau, "tau_range": e["tau_min_range"], "gain_c_per_duty":
            (power_w / g_hi, power_w / g_lo), "heat_capacity_kj_k":
            (e["tau_min_range"][0] * 60 * g_lo / 1000, e["tau_min_range"][1] * 60 * g_hi / 1000),
            "rows": rows, "start_c": start_c, "target_c": target_c}


def render(ambient_c=None, rref=82, idle=None, mains_v=None, rated_v=None,
           element_g=ELEMENT_G, airflow=AIRFLOW_M3_MIN, power_w=POWER_W):
    """Markdown lines of all priors."""
    L = ["## Priors (expected values, estimates)", ""]
    L.append("These numbers come from physics and from known data, before the measurement. "
             "Each one has assumptions. If a capture disagrees, the capture wins. "
             "Then find out which assumption is wrong.")
    amb = 22.0 if ambient_c is None else ambient_c
    L += ["", f"### Thermistor chain at {amb:.1f} C ambient (firmware Rref {rref} k)", "",
          "| Mistake | Displayed | Note |", "|---|---|---|"]
    for name, disp, note in forensics(amb, rref):
        L.append(f"| {name} | {'--' if disp is None else f'{disp:.1f} C'} | {note} |")
    if idle:
        inv = inverse(idle[0], idle[1], ambient_c, rref)
        L += ["", f"### Inverse: idle chamber {idle[0]:.1f} C, PTC {idle[1]:.1f} C "
              f"(reference: {inv['reference']} {inv['reference_c']:.1f} C)", ""]
        for name, c in inv["channels"].items():
            if c["ratio"] is None:
                L.append(f"- {name}: {c['fits'][0]}")
                continue
            L.append(f"- {name}: resistance ratio expected/displayed = {c['ratio']:.2f} "
                     f"(82/33 = {82 / 33:.2f}), implied Rref {c['implied_rref_kohm']:.0f} k. "
                     "Hypotheses that fit:")
            L += [f"  - {f}" for f in c["fits"]]
    fh = first_heat(power_w, element_g)
    L += ["", "### First heat: element rise before airflow losses", "",
          f"Assumption: {power_w:.0f} W into aluminium (c = {ALU_C_J_PER_G_K} J/(g K)). "
          "The element NTC and the airflow make the measured rise smaller.", "",
          "| Element mass | Rate | Rise in 12 s | Margin to 3 C |", "|---|---|---|---|"]
    for r in fh["rows"]:
        L.append(f"| {r['element_g']:.0f} g | {r['rate_c_s']:.2f} C/s | {r['rise_12s_c']:.0f} C | "
                 f"{r['margin_x']:.0f} x |")
    L.append(f"\nData point, issue #122 under heat: PTC 63.5 -> 98.4 C in about 24 s = "
             f"{fh['issue122_rate_c_s']:.2f} C/s (a hot start, with losses). That rate fits an "
             f"effective {fh['issue122_effective_g']:.0f} g. A 3 C rise in 12 s needs only "
             f"{FH_PASS_C / FH_WINDOW_S:.2f} C/s.")
    L += ["", "### Outlet air rise (chamber NTC in the outlet stream)", "",
          f"Assumption: airflow not measured, rho {AIR_RHO} kg/m3, cp {AIR_CP:.0f} J/(kg K), "
          f"capped at the {FOLDBACK_CUT_C:.0f} C foldback (PTC self-limit).", "",
          "| Airflow | Steady outlet rise | Capped |", "|---|---|---|"]
    for r in outlet(power_w, airflow):
        L.append(f"| {r['airflow_m3_min']:.2f} m3/min | {r['rise_c']:.0f} C | "
                 f"{r['capped_c']:.0f} C{' (cap)' if r['capped'] else ''} |")
    L.append("\nThe steady outlet rise is far above the 3 C crossed limit. Thus the crossed rule "
             "depends on the lag: the element NTC must rise faster than the outlet NTC in the "
             "first 12-30 s. A first-heat capture must confirm that the chamber rise stays below "
             "half the PTC rise in that window.")
    L += ["", "### Mains voltage (input, not a guess)", ""]
    cases = [(mains_v, rated_v or mains_v)] if mains_v else [(120.0, 230.0), (230.0, 230.0)]
    for mv, rv in cases:
        m = mains(mv, rv, power_w)
        L.append(f"- mains {mv:.0f} V on a {rv:.0f} V unit: PTC about {m['ptc_power_w']:.0f} W "
                 f"(V^2 scaling, cold). Fan ({FAN_RATED_V:.0f} VAC EC blower): {m['fan']}.")
    if not mains_v:
        L.append("- Read the rating on the unit label and give --mains-v and --rated-v.")
    en = enclosure(power_w)
    L += ["", "### CORE One L enclosure (printer chamber sensor, no chamber heater)", "",
          f"Cooldown time constant: median {en['tau_min']:.0f} min (range "
          f"{en['tau_range'][0]:.0f}-{en['tau_range'][1]:.0f} min, {ENCLOSURE['cooldowns']} "
          "cooldowns). Loss 8-12 W/K is an ESTIMATE (assumed 100-150 W of bed heat at bed 85 C).",
          f"Expected FOPDT gain: {en['gain_c_per_duty'][0]:.0f}-{en['gain_c_per_duty'][1]:.0f} C "
          f"per unit duty. Heat capacity {en['heat_capacity_kj_k'][0]:.0f}-"
          f"{en['heat_capacity_kj_k'][1]:.0f} kJ/K. A step needs about 3 tau, 2.5 h.",
          "The local chamber NTC sits in the outlet: its fit gives a larger gain and a "
          "faster part than the printer sensor.", "",
          f"| Bed heat into chamber | Loss | Steady rise | {en['start_c']:.0f} -> "
          f"{en['target_c']:.0f} C |", "|---|---|---|---|"]
    for r in en["rows"]:
        t = "not reached" if r["minutes"] is None else f"{r['minutes']:.0f} min"
        L.append(f"| {r['bed_w']:.0f} W | {r['loss_w_k']:.0f} W/K | {r['steady_rise_c']:.0f} C | {t} |")
    return L


def main(argv=None):
    p = argparse.ArgumentParser(description="Expected values for a DragonBreath capture.")
    p.add_argument("--ambient-c", type=float, help="room temperature from a thermometer")
    p.add_argument("--rref", type=int, default=82, choices=(82, 33), help="rref_kohm from /api/v2/info")
    p.add_argument("--idle", type=float, nargs=2, metavar=("CHAMBER_C", "PTC_C"),
                   help="idle reading pair after a long time with no heat")
    p.add_argument("--mains-v", type=float, help="mains voltage at the outlet")
    p.add_argument("--rated-v", type=float, help="rated voltage on the unit label")
    p.add_argument("--element-g", type=float, nargs=2, default=ELEMENT_G)
    p.add_argument("--airflow", type=float, nargs=2, default=AIRFLOW_M3_MIN, help="m3/min range")
    p.add_argument("--power-w", type=float, default=POWER_W)
    a = p.parse_args(argv)
    print("\n".join(render(a.ambient_c, a.rref, a.idle, a.mains_v, a.rated_v,
                           a.element_g, a.airflow, a.power_w)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
