#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline analysis of a DragonBreath capture (tools/capture.py, tools/diag.py CSV).

  python3 tools/analyze_capture.py CAPTURE [--ambient-c C] [--mains-v V --rated-v V]
  python3 tools/analyze_capture.py CAPTURE --write-expect   # make a regression test

It prints the priors (expected values, tools/priors.py) first, then the measured
values, and writes analysis.md and analysis.json next to the capture.

Sections:
  first heat       PTC and chamber rise at 12 s and 30 s of relay ON time, the
                   provisional rule, suggested thresholds with margin
  cold agreement   PTC - chamber at idle, its stability, thermistor-chain hypotheses
  airflow          outlet/PTC rise ratio at each heat start, foldback cycles per hour,
                   PTC cooldown time constant with the fan on
  step response    first-order-plus-dead-time fit, SIMC and Cohen-Coon PID gains,
                   overshoot and settling of the recorded control
  control health   late ticks, tick times, heap and stack minima from /metrics
  replay           tools/replay_capture.py
Each section uses only the columns that exist. Missing fields give a note, not an error.
Standard library only.
"""
import argparse
import json
import math
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dbcapture_common as common  # noqa: E402
import heater_model as hm  # noqa: E402
import ntc_model as ntc  # noqa: E402
import priors  # noqa: E402
import replay_capture  # noqa: E402

PID_WINDOW_S = 30.0          # PB_HEATER_PID_WINDOW_US
CURRENT_PID = (0.1, 0.001, 0.04)   # PB_HEATER_PID_KP/KI/KD
COLD_SETTLE_S = 1800.0       # no heat for this long before a cold-agreement sample
COLD_LIMIT_C = 3.0
HEAT_START_OFF_S = 60.0
AIRFLOW_WINDOW_S = 30.0
SETTLE_BAND_C = 1.5
STEP_MIN_S = 2.5 * 3600      # about 3 tau of the CORE One L enclosure


# ---------------------------------------------------------------------------- helpers

def interp(ts, vs, t):
    if not ts:
        return None
    if t <= ts[0]:
        return vs[0]
    if t >= ts[-1]:
        return vs[-1]
    lo, hi = 0, len(ts) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if ts[mid] <= t:
            lo = mid
        else:
            hi = mid
    f = (t - ts[lo]) / (ts[hi] - ts[lo]) if ts[hi] > ts[lo] else 0.0
    return vs[lo] + f * (vs[hi] - vs[lo])


def series(rows, col):
    ts, vs = [], []
    for r in rows:
        if r.get(col) is not None:
            ts.append(r["t_s"])
            vs.append(r[col])
    return ts, vs


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else None


def window_mean(rows, col, t0, t1):
    return mean([r.get(col) for r in rows if t0 <= r["t_s"] < t1])


def relay_runs(rows):
    """[(value, t_start, t_end, i_start)] for the relay column."""
    runs = []
    for i, r in enumerate(rows):
        v = r.get("relay")
        if v is None:
            continue
        if runs and runs[-1][0] == v:
            runs[-1][2] = r["t_s"]
        else:
            if runs:
                runs[-1][2] = r["t_s"]
            runs.append([v, r["t_s"], r["t_s"], i])
    return [tuple(x) for x in runs]


def on_time_reach(rows, i0, seconds, wall_max=None):
    """Wall time at which cumulative relay ON time from row i0 reaches `seconds`."""
    on = 0.0
    for i in range(i0 + 1, len(rows)):
        prev, cur = rows[i - 1], rows[i]
        dt = cur["t_s"] - prev["t_s"]
        if wall_max is not None and cur["t_s"] - rows[i0]["t_s"] > wall_max:
            return None
        if prev.get("relay") == 1:
            if on + dt >= seconds:
                return prev["t_s"] + (seconds - on)
            on += dt
    return None


def exp_fit(ts, ys):
    """Fit y = A + B exp(-(t - t0)/tau). Return dict(tau_s, asymptote_c, rmse_c) or None."""
    if len(ts) < 8:
        return None
    t0 = ts[0]
    x = [t - t0 for t in ts]

    def sse_for(tau):
        e = [math.exp(-xi / tau) for xi in x]
        n = len(e)
        se, sy = sum(e), sum(ys)
        see = sum(v * v for v in e)
        sey = sum(a * b for a, b in zip(e, ys))
        den = n * see - se * se
        if abs(den) < 1e-12:
            return float("inf"), 0.0, 0.0
        b = (n * sey - se * sy) / den
        a = (sy - b * se) / n
        return sum((a + b * ei - y) ** 2 for ei, y in zip(e, ys)), a, b

    span = max(x[-1], 1.0)
    taus = [2.0 * (1.15 ** k) for k in range(90) if 2.0 * (1.15 ** k) < 50 * span]
    best = min(taus, key=lambda tau: sse_for(tau)[0])
    lo, hi = math.log(best / 1.15), math.log(best * 1.15)
    for _ in range(40):          # golden section on log(tau)
        m1, m2 = lo + 0.382 * (hi - lo), lo + 0.618 * (hi - lo)
        if sse_for(math.exp(m1))[0] < sse_for(math.exp(m2))[0]:
            hi = m2
        else:
            lo = m1
    tau = math.exp((lo + hi) / 2)
    sse, a, b = sse_for(tau)
    return {"tau_s": tau, "asymptote_c": a, "amplitude_c": b,
            "rmse_c": math.sqrt(sse / len(ts)), "span_s": span}


def resample(rows, col, t0, t1, dt, hold=False):
    ts, vs = series([r for r in rows if t0 - 60 <= r["t_s"] <= t1 + 60], col)
    out, t, j = [], t0, -1
    while t <= t1 + 1e-9:
        if hold:
            while j + 1 < len(ts) and ts[j + 1] <= t:
                j += 1
            out.append(vs[max(j, 0)] if vs else 0)
        else:
            out.append(interp(ts, vs, t))
        t += dt
    return out


def _zoh_sim(u, a):
    x, xs = 0.0, []
    for ui in u:
        x = a * x + (1 - a) * ui
        xs.append(x)
    return xs


def fopdt_fit(u, y, dt, y0=None):
    """Fit y - y0 = K * x, tau x' = -x + u(t - theta). Grid search on theta and tau,
    least squares for K. Return dict(K, tau_s, theta_s, r2) or None."""
    n = len(y)
    if n < 20 or max(u) <= 0:
        return None
    y0 = y[0] if y0 is None else y0
    dy = [v - y0 for v in y]
    var = sum((v - mean(dy)) ** 2 for v in dy) or 1e-9
    dur = n * dt

    def score(theta_steps, tau):
        us = [0.0] * theta_steps + u[:n - theta_steps]
        xs = _zoh_sim(us, math.exp(-dt / tau))
        sxx = sum(v * v for v in xs)
        if sxx <= 1e-12:
            return float("inf"), 0.0
        k = sum(a * b for a, b in zip(xs, dy)) / sxx
        return sum((b - k * a) ** 2 for a, b in zip(xs, dy)), k

    def best_tau(th):
        lo, hi = math.log(5.0), math.log(max(50.0, 30 * dur))
        grid = [lo + (hi - lo) * i / 24 for i in range(25)]
        g = min(grid, key=lambda lt: score(th, math.exp(lt))[0])
        lo, hi = g - (hi - lo) / 24, g + (hi - lo) / 24
        for _ in range(30):          # golden section on log(tau)
            m1, m2 = lo + 0.382 * (hi - lo), lo + 0.618 * (hi - lo)
            if score(th, math.exp(m1))[0] < score(th, math.exp(m2))[0]:
                hi = m2
            else:
                lo = m1
        tau = math.exp((lo + hi) / 2)
        s, k = score(th, tau)
        return s, k, th, tau

    max_th = int(min(300.0, dur / 4) / dt)
    step = max(1, max_th // 20)
    best = min((best_tau(th) for th in range(0, max_th + 1, step)), key=lambda b: b[0])
    th0 = best[2]
    for th in range(max(0, th0 - step + 1), min(max_th, th0 + step - 1) + 1):
        if th != th0:
            best = min(best, best_tau(th), key=lambda b: b[0])
    s, k, th, tau = best
    return {"K_c_per_duty": k, "tau_s": tau, "theta_s": th * dt, "r2": 1 - s / var,
            "dt_s": dt, "duration_s": dur, "tau_identifiable": dur >= 3 * tau}


def tune(fit, extra_dead_s):
    """SIMC PI (Skogestad 2003) for tau_c = theta and 3 theta, plus Cohen-Coon PID.
    Firmware form: duty = Kp e + Ki integral(e dt) + Kd d(-y)/dt, duty 0..1."""
    K, tau = fit["K_c_per_duty"], fit["tau_s"]
    th = fit["theta_s"] + extra_dead_s
    if K <= 0 or tau <= 0:
        return None
    out = {"theta_eff_s": th}
    for name, tc in (("simc_tight", th), ("simc_smooth", 3 * th)):
        kc = tau / (K * (tc + th))
        ti = min(tau, 4 * (tc + th))
        out[name] = {"kp": kc, "ki": kc / ti, "kd": 0.0, "tau_c_s": tc}
    r = th / tau
    kc = (1 / K) * (1 / r) * (4 / 3 + r / 4)
    ti = th * (32 + 6 * r) / (13 + 8 * r)
    td = 4 * th / (11 + 2 * r)
    out["cohen_coon"] = {"kp": kc, "ki": kc / ti, "kd": kc * td}
    return out


def fmt(v, spec=".1f", none="--"):
    return none if v is None else format(v, spec)


# ---------------------------------------------------------------------------- sections

def section_summary(cap):
    rows = cap.rows
    dts = [b["t_s"] - a["t_s"] for a, b in zip(rows, rows[1:])]
    present = [c for c in common.STATE_COLUMNS if cap.has(c)]
    h = cap.header
    return {
        "kind": cap.kind, "samples": len(rows),
        "duration_s": rows[-1]["t_s"] - rows[0]["t_s"] if rows else 0.0,
        "median_dt_s": statistics.median(dts) if dts else None,
        "firmware": (h.get("info") or {}).get("firmware"), "rref_kohm": cap.rref_kohm,
        "scenario": h.get("scenario"), "synthetic": h.get("synthetic"),
        "notes": h.get("notes"), "method": h.get("heater_method"),
        "columns_missing": [c for c in common.STATE_COLUMNS if c not in present],
        "metrics_snapshots": len(cap.metrics),
    }


def section_first_heat(cap):
    rows = cap.rows
    runs = relay_runs(rows)
    first = None
    for k, (v, t0, t1, i0) in enumerate(runs):
        if v == 1:
            prior_off = (runs[k - 1][2] - runs[k - 1][1]) if k else 0.0
            if k and runs[k - 1][0] == 0 and prior_off >= 10.0:
                first = (t0, i0, prior_off, k == 1)
            break
    if not first:
        return {"found": False, "why": "no relay ON with at least 10 s of OFF data before it"}
    t_on, i_on, prior_off, first_in_capture = first
    base_p = window_mean(rows, "ptc_c", t_on - 10, t_on + 0.01)
    base_c = window_mean(rows, "chamber_c", t_on - 10, t_on + 0.01)
    noise = [r["ptc_c"] for r in rows if t_on - 60 <= r["t_s"] < t_on and r.get("ptc_c") is not None]
    sigma = statistics.pstdev(noise) if len(noise) > 2 else None
    out = {"found": True, "t_on_s": t_on, "off_before_s": prior_off,
           "cold": base_p is not None and base_c is not None and abs(base_p - base_c) < 5.0,
           "ptc_start_c": base_p, "chamber_start_c": base_c, "ptc_noise_sigma_c": sigma}
    ts_p, vs_p = series(rows, "ptc_c")
    ts_c, vs_c = series(rows, "chamber_c")
    for sec in (12, 30):
        t = on_time_reach(rows, i_on, sec, wall_max=hm.FH_MAX_WALL_MS / 1000)
        if t is None or base_p is None or base_c is None:
            out[f"at_{sec}s"] = None
            continue
        pr = interp(ts_p, vs_p, t) - base_p
        cr = interp(ts_c, vs_c, t) - base_c
        verdict = hm.fh_eval(sec * 1000, hm.c_to_dc(pr), hm.c_to_dc(cr),
                             max_on_ms=sec * 1000)
        out[f"at_{sec}s"] = {"wall_s": t - t_on, "ptc_rise_c": pr, "ch_rise_c": cr,
                             "ratio_ptc_ch": pr / cr if abs(cr) > 0.05 else None,
                             "rule": verdict}
    # whole-window decision with the firmware-like model (peak minus start, ON time)
    fh = replay_capture.replay_first_heat(cap)
    out["model"] = fh
    a12 = out.get("at_12s")
    if a12 and a12["ptc_rise_c"] > 0:
        p12 = a12["ptc_rise_c"]
        floor = 0.5 if sigma is None else max(0.5, 5 * sigma)
        sug_pass = max(floor, math.floor(0.5 * p12 * 2) / 2)
        a30 = out.get("at_30s") or a12
        ch_max = max(a12["ch_rise_c"], a30["ch_rise_c"])
        sug_cross = max(sug_pass, math.ceil(2 * max(ch_max, 0.25) * 2) / 2)
        out["suggest"] = {
            "PB_FH_PASS_RISE_DC": int(round(sug_pass * 10)),
            "PB_FH_CROSS_RISE_DC": int(round(sug_cross * 10)),
            "pass_margin_x": p12 / sug_pass if sug_pass else None,
            "cross_margin_x": sug_cross / ch_max if ch_max > 0 else None,
            "noise_floor_c": floor,
            "rule": "PASS limit = half of the PTC rise at 12 s (not below 5 sigma noise). "
                    "CROSSED limit = twice the largest normal chamber rise, not below the PASS limit.",
        }
        if sug_cross > p12:
            out["suggest"]["warning"] = ("the normal chamber rise is too close to the PTC rise: "
                                         "the CROSSED limit is above the PTC rise at 12 s")
    return out


def _idle_rows(cap):
    rows = cap.rows
    last_on = None
    out = []
    for r in rows:
        if r.get("relay") == 1:
            last_on = r["t_s"]
            continue
        settled = last_on is None or r["t_s"] - last_on >= COLD_SETTLE_S
        fan_off = r.get("fan_pct") in (None, 0, 0.0)
        if settled and fan_off and r.get("chamber_c") is not None and r.get("ptc_c") is not None:
            out.append(r)
    return out


def section_cold(cap, ambient_c=None):
    idle = _idle_rows(cap)
    if len(idle) < 20 or idle[-1]["t_s"] - idle[0]["t_s"] < 60:
        return {"found": False, "why": "less than 60 s of idle data (relay off, fan off, "
                f"no heat in the last {COLD_SETTLE_S / 60:.0f} min)"}
    diffs = [r["ptc_c"] - r["chamber_c"] for r in idle]
    chs = [r["chamber_c"] for r in idle]
    pts = [r["ptc_c"] for r in idle]
    hrs = (idle[-1]["t_s"] - idle[0]["t_s"]) / 3600
    med = statistics.median(diffs)
    ref = ambient_c
    ref_src = "argument" if ambient_c is not None else None
    if ref is None and cap.header.get("ambient_c") is not None:
        ref, ref_src = float(cap.header["ambient_c"]), "capture header"
    if ref is None and cap.has("printer_chamber_c"):
        ref = statistics.median([r["printer_chamber_c"] for r in idle
                                 if r.get("printer_chamber_c") is not None] or [None]) or None
        ref_src = "printer chamber sensor" if ref is not None else None
    rref = cap.rref_kohm or 82
    other = 33 if rref == 82 else 82
    ch_med, p_med = statistics.median(chs), statistics.median(pts)
    out = {
        "found": True, "samples": len(idle), "hours": hrs,
        "diff_median_c": med, "diff_p5_c": sorted(diffs)[len(diffs) // 20],
        "diff_p95_c": sorted(diffs)[len(diffs) * 19 // 20], "diff_std_c": statistics.pstdev(diffs),
        "diff_slope_c_per_h": ((mean(diffs[-len(diffs) // 4:]) - mean(diffs[:len(diffs) // 4]))
                               / (0.75 * hrs) if hrs > 0 else None),
        "chamber_median_c": ch_med, "ptc_median_c": p_med,
        "chamber_pp_c": max(chs) - min(chs), "ptc_pp_c": max(pts) - min(pts),
        "reference_c": ref, "reference_source": ref_src, "rref_kohm": rref,
        "with_other_rref": {"rref_kohm": other,
                            "chamber_c": ntc.reinterpret_c(ch_med, rref, other),
                            "ptc_c": ntc.reinterpret_c(p_med, rref, other)},
        "agree": abs(med) <= COLD_LIMIT_C,
        "oscillation": max(pts) - min(pts) > 3.0 and max(chs) - min(chs) < 1.5,
    }
    out["chain"] = priors.inverse(ch_med, p_med, ref, rref)
    hint = []
    if ref is not None:
        o = out["with_other_rref"]
        if (abs(ch_med - ref) > COLD_LIMIT_C and abs(o["chamber_c"] - ref) <= 2.0
                and abs(o["ptc_c"] - ref) <= 2.0):
            hint.append(f"Both readings fit the reference with Rref {other} k: the firmware "
                        f"probably uses the wrong Rref ({rref} k).")
    elif max(ch_med, p_med) > 40 or min(ch_med, p_med) <= ntc.T_MIN_C + 0.05:
        hint.append("An idle reading above 40 C or at the 12 C table end is not plausible "
                    "for a room. Give --ambient-c to test the wrong-Rref hypothesis.")
    out["rref_hint"] = hint
    if ref is not None and out["agree"] and not hint:
        out["suggest_offsets"] = {
            "chamber_offset_c": max(-5.0, min(5.0, round(ref - ch_med, 1))),
            "ptc_offset_c": max(-5.0, min(5.0, round(ref - p_med, 1)))}
    elif out["agree"]:
        out["suggest_offsets"] = {"chamber_offset_c": 0.0,
                                  "ptc_offset_c": max(-5.0, min(5.0, round(-med, 1)))}
    return out


def _heating_hours(rows):
    h = 0.0
    for a, b in zip(rows, rows[1:]):
        armed = (a.get("target_c") or 0) > 0 if a.get("target_c") is not None else (
            a.get("mode") in common.HEAT_MODES)
        if armed:
            h += b["t_s"] - a["t_s"]
    return h / 3600


def section_airflow(cap):
    rows = cap.rows
    runs = relay_runs(rows)
    starts = []
    ts_p, vs_p = series(rows, "ptc_c")
    ts_c, vs_c = series(rows, "chamber_c")
    for k, (v, t0, t1, i0) in enumerate(runs):
        if v != 1 or not k or runs[k - 1][0] != 0 or runs[k - 1][2] - runs[k - 1][1] < HEAT_START_OFF_S:
            continue
        t = on_time_reach(rows, i0, AIRFLOW_WINDOW_S, wall_max=3 * AIRFLOW_WINDOW_S)
        if t is None or not ts_p or not ts_c:
            continue
        pr = interp(ts_p, vs_p, t) - interp(ts_p, vs_p, t0)
        cr = interp(ts_c, vs_c, t) - interp(ts_c, vs_c, t0)
        starts.append({"t_s": t0, "ptc_rise_c": pr, "ch_rise_c": cr,
                       "ratio": cr / pr if pr > 0.5 else None})
    ratios = [s["ratio"] for s in starts if s["ratio"] is not None]
    hours = _heating_hours(rows)
    rref = cap.rref_kohm or 82
    fbc = float((cap.header.get("settings") or {}).get("fb_cut") or 0)
    cut, _ = hm.foldback_thresholds(rref, fbc)
    if cap.has("constraint"):
        fb_count = sum(1 for a, b in zip(rows, rows[1:]) if b.get("constraint") == "element_foldback"
                       and a.get("constraint") != "element_foldback")
        fb_src = "heater.constraint"
    else:
        fb_count = sum(1 for a, b in zip(rows, rows[1:])
                       if a.get("relay") == 1 and b.get("relay") == 0 and (b.get("ptc_c") or 0) >= cut - 1.0
                       and (b.get("target_c") is None or (b.get("chamber_c") or 0) < b["target_c"]))
        fb_src = f"inferred (relay off with PTC >= {cut - 1:.0f} C below target)"
    # PTC cooldown with the fan on: each relay OFF run after an ON run
    taus = []
    for k, (v, t0, t1, i0) in enumerate(runs):
        if v != 0 or not k or runs[k - 1][0] != 1 or runs[k - 1][2] - runs[k - 1][1] < 20:
            continue
        if t1 - t0 < 20:
            continue
        seg = [r for r in rows if t0 + 3 <= r["t_s"] <= min(t1, t0 + 120)
               and r.get("ptc_c") is not None]
        if any(r.get("fan_pct") == 0 for r in seg):
            continue
        if len(seg) < 10 or seg[0]["ptc_c"] - seg[-1]["ptc_c"] < 3.0:
            continue
        f = exp_fit([r["t_s"] for r in seg], [r["ptc_c"] for r in seg])
        if f and f["tau_s"] < 1000:
            taus.append(f["tau_s"])
    heating = [r["ptc_c"] for r in rows if r.get("relay") == 1 and r.get("ptc_c") is not None]
    return {
        "heat_starts": starts, "ratio_median": statistics.median(ratios) if ratios else None,
        "heating_hours": hours, "foldback_cycles": fb_count, "foldback_source": fb_src,
        "foldback_per_hour": fb_count / hours if hours > 0.05 else None,
        "foldback_cut_c": cut,
        "ptc_cool_tau_s_median": statistics.median(taus) if taus else None,
        "ptc_cool_tau_count": len(taus),
        "ptc_max_heating_c": max(heating) if heating else None,
        "fan_column": cap.has("fan_pct"),
    }


def section_step(cap):
    rows = cap.rows
    segs, cur = [], None
    for r in rows:
        tgt = r.get("target_c")
        if tgt is not None and tgt > 0:
            if cur and cur["target"] == tgt:
                cur["t1"] = r["t_s"]
            else:
                cur = {"target": tgt, "t0": r["t_s"], "t1": r["t_s"]}
                segs.append(cur)
        else:
            cur = None
    segs = [s for s in segs if s["t1"] - s["t0"] >= 300]
    if not segs or not cap.has("relay") or not cap.has("chamber_c"):
        return {"found": False, "why": "no heat segment of 5 min or more with a constant target"}
    s = max(segs, key=lambda x: x["t1"] - x["t0"])
    target, t0, t1 = s["target"], s["t0"], s["t1"]
    pre = max(rows[0]["t_s"], t0 - 120)
    dur = t1 - pre
    dt = max(2.0, dur / 900)
    u = resample(rows, "relay", pre, t1, dt, hold=True)
    out = {"found": True, "target_c": target, "t_start_s": t0, "duration_s": t1 - t0,
           "long_enough": t1 - t0 >= STEP_MIN_S}
    fits = {}
    for col in ("chamber_c", "printer_chamber_c"):
        if not cap.has(col):
            continue
        y = resample(rows, col, pre, t1, dt)
        if any(v is None for v in y):
            continue
        y0 = window_mean(rows, col, pre, t0 + 0.01) or y[0]
        f = fopdt_fit([float(v) for v in u], y, dt, y0=y0)
        if f:
            f["tuning"] = tune(f, PID_WINDOW_S / 2 + dt / 2)
            fits[col] = f
    out["fits"] = fits
    seg = [r for r in rows if t0 <= r["t_s"] <= t1 and r.get("chamber_c") is not None]
    reach = next((r["t_s"] for r in seg if r["chamber_c"] >= target), None)
    out["reach_s"] = None if reach is None else reach - t0
    if reach is not None:
        after = [r for r in seg if r["t_s"] >= reach]
        out["overshoot_c"] = max(r["chamber_c"] for r in after) - target
        last_out = None
        for r in after:
            if abs(r["chamber_c"] - target) > SETTLE_BAND_C:
                last_out = r["t_s"]
        out["settle_s"] = (reach if last_out is None else last_out) - t0
        out["settled"] = last_out is None or last_out < t1 - 300
        hold = after[len(after) // 2:]
        if len(hold) > 10:
            hrs = (hold[-1]["t_s"] - hold[0]["t_s"]) / 3600
            ons = sum(1 for a, b in zip(hold, hold[1:]) if a.get("relay") == 0 and b.get("relay") == 1)
            out["hold"] = {"mean_c": mean([r["chamber_c"] for r in hold]),
                           "pp_c": max(r["chamber_c"] for r in hold) - min(r["chamber_c"] for r in hold),
                           "duty": mean([r.get("relay") for r in hold]),
                           "relay_cycles_per_hour": ons / hrs if hrs > 0 else None}
    rise = [r for r in seg if reach is None or r["t_s"] <= reach]
    rates = []
    for a in range(0, len(rise)):
        b = a
        while b < len(rise) and rise[b]["t_s"] - rise[a]["t_s"] < 60:
            b += 1
        if b < len(rise):
            rates.append((rise[b]["chamber_c"] - rise[a]["chamber_c"]) / (rise[b]["t_s"] - rise[a]["t_s"]))
    out["max_rise_rate_c_per_s"] = max(rates) if rates else None
    return out


def section_health(cap):
    if not cap.metrics:
        return {"found": False, "why": "no /metrics snapshots (the firmware has no /metrics, "
                "or the capture did not ask)"}
    M = common.metric
    first, last = cap.metrics[0][1], cap.metrics[-1][1]

    def series_of(name, **lab):
        return [v for _, m in cap.metrics if (v := M(m, name, **lab)) is not None]

    def delta(name):
        a, b = M(first, name), M(last, name)
        return None if a is None or b is None else b - a

    stacks = {}
    for _, m in cap.metrics:
        for lab, v in m.get("dragonbreath_task_stack_free_minimum_bytes", ()):
            t = lab.get("task", "?")
            stacks[t] = min(stacks.get(t, v), v)
    ups = series_of("dragonbreath_uptime_seconds")
    reboots = sum(1 for a, b in zip(ups, ups[1:]) if b < a)
    out = {"found": True, "snapshots": len(cap.metrics),
           "late_ticks_delta": delta("dragonbreath_control_ticks_late_total"),
           "ticks_delta": delta("dragonbreath_control_ticks_total"),
           "tick_max_s": max(series_of("dragonbreath_control_tick_duration_max_seconds") or [None],
                             key=lambda x: -1 if x is None else x),
           "interval_max_s": max(series_of("dragonbreath_control_tick_interval_max_seconds") or [None],
                                 key=lambda x: -1 if x is None else x),
           "heap_min_bytes": min(series_of("dragonbreath_heap_minimum_free_bytes") or [None],
                                 key=lambda x: 1e12 if x is None else x),
           "largest_block_min_bytes": min(series_of("dragonbreath_heap_largest_free_block_bytes")
                                          or [None], key=lambda x: 1e12 if x is None else x),
           "stack_min_bytes": stacks, "reboots": reboots,
           "relay_on_s_delta": delta("dragonbreath_relay_on_seconds_total")}
    flags = []
    if (out["late_ticks_delta"] or 0) > 0:
        flags.append(f"{out['late_ticks_delta']:.0f} late control ticks")
    if out["tick_max_s"] is not None and out["tick_max_s"] > 0.05:
        flags.append(f"longest tick {1000 * out['tick_max_s']:.1f} ms (> 50 ms)")
    if out["interval_max_s"] is not None and out["interval_max_s"] > 0.75:
        flags.append(f"longest tick interval {1000 * out['interval_max_s']:.0f} ms (> 750 ms)")
    if out["heap_min_bytes"] is not None and out["heap_min_bytes"] < 20000:
        flags.append(f"heap minimum {out['heap_min_bytes']:.0f} B (< 20 KB)")
    for t, v in stacks.items():
        if v < 512:
            flags.append(f"stack of {t}: {v:.0f} B free (< 512 B)")
    if reboots:
        flags.append(f"{reboots} reboot(s) during the capture")
    out["flags"] = flags
    return out


def analyze(cap, ambient_c=None):
    res = {"summary": section_summary(cap)}
    res["first_heat"] = section_first_heat(cap)
    res["cold"] = section_cold(cap, ambient_c)
    res["airflow"] = section_airflow(cap)
    res["step"] = section_step(cap)
    res["health"] = section_health(cap)
    rp = replay_capture.replay(cap)
    res["replay"] = {"ok": rp["ok"], "violations": len(rp["violations"]),
                     "unexplained": rp["decisions"].get("unexplained"),
                     "modeled": rp["decisions"].get("modeled")}
    res["_replay_full"] = rp
    faults = [r for r in cap.rows if r.get("fault") == 1]
    res["faults"] = {"samples": len(faults), "first_t_s": faults[0]["t_s"] if faults else None,
                     "reasons": sorted({r.get("reason") or "?" for r in faults})}
    res["flags"] = flags(res)
    return res


def flags(res):
    F = []
    if res["faults"]["samples"]:
        F.append(("WARN", f"a fault is latched from t = {res['faults']['first_t_s']:.1f} s: "
                  + ", ".join(res["faults"]["reasons"])))
    fh = res["first_heat"]
    if fh.get("found"):
        m = fh["model"]["model_state"]
        if m in ("crossed", "no_rise"):
            F.append(("FAIL", f"first heat: the provisional rule gives {m.upper()}"))
        if (fh.get("suggest") or {}).get("warning"):
            F.append(("WARN", "first heat: " + fh["suggest"]["warning"]))
    c = res["cold"]
    if c.get("found"):
        if not c["agree"]:
            F.append(("WARN", f"cold agreement: PTC - chamber = {c['diff_median_c']:+.1f} C "
                      f"(limit {COLD_LIMIT_C:.0f} C)"))
        if c["oscillation"]:
            F.append(("WARN", f"cold agreement: the idle PTC moves {c['ptc_pp_c']:.1f} C while the "
                      "chamber is steady (upstream issue #122 pattern)"))
        for h in c["rref_hint"]:
            F.append(("WARN", "thermistor chain: " + h))
    a = res["airflow"]
    if a["ratio_median"] is not None and a["ratio_median"] < 0.05:
        F.append(("WARN", f"airflow: outlet/PTC rise ratio {a['ratio_median']:.3f} is very low "
                  "(stalled fan, blocked inlet, or a sensor out of the stream)"))
    if a["foldback_per_hour"] is not None and a["foldback_per_hour"] > 6:
        F.append(("WARN", f"airflow: {a['foldback_per_hour']:.0f} foldback cycles per hour of heat"))
    s = res["step"]
    if s.get("found"):
        f = s["fits"].get("chamber_c")
        if f and f["K_c_per_duty"] < 2.0:
            F.append(("FAIL", f"step: gain {f['K_c_per_duty']:.1f} C per duty: the heater adds "
                      "almost no heat"))
        if not s["long_enough"]:
            F.append(("NOTE", f"step: {s['duration_s'] / 3600:.1f} h of heat. The enclosure needs "
                      "about 2.5 h (3 tau) for a reliable time constant."))
    for x in res["health"].get("flags", []):
        F.append(("WARN", "health: " + x))
    if not res["replay"]["ok"]:
        F.append(("FAIL", "replay: a safety invariant or the decision check failed"))
    return F


# ---------------------------------------------------------------------------- report

def numbers_table(res):
    """(quantity, value, firmware destination)."""
    T = []
    fh = res["first_heat"]
    sg = fh.get("suggest") if fh.get("found") else None
    if sg:
        T.append(("first-heat PASS rise", f"{sg['PB_FH_PASS_RISE_DC']} dC",
                  "PB_FH_PASS_RISE_DC, components/pb_heater/include/pb_heater.h (feat/first-heat-check)"))
        T.append(("first-heat CROSSED rise", f"{sg['PB_FH_CROSS_RISE_DC']} dC",
                  "PB_FH_CROSS_RISE_DC, same file"))
    c = res["cold"]
    if c.get("found"):
        T.append(("idle PTC - chamber", f"{c['diff_median_c']:+.1f} C (std {c['diff_std_c']:.2f})",
                  "limit of the planned cold sensor-agreement check (new constant)"))
        if c.get("suggest_offsets"):
            o = c["suggest_offsets"]
            T.append(("calibration offsets", f"chamber {o['chamber_offset_c']:+.1f} C, "
                      f"PTC {o['ptc_offset_c']:+.1f} C",
                      "runtime: POST /api/v2/calibration (not a constant, max +-5 C)"))
    a = res["airflow"]
    if a["ratio_median"] is not None:
        T.append(("outlet/PTC rise ratio", f"{a['ratio_median']:.3f}",
                  "baseline of the planned airflow sign (/metrics)"))
    if a["foldback_per_hour"] is not None:
        T.append(("foldback cycles per hour", f"{a['foldback_per_hour']:.1f}",
                  "baseline of the planned airflow sign"))
    if a["ptc_cool_tau_s_median"] is not None:
        T.append(("PTC cooldown tau, fan on", f"{a['ptc_cool_tau_s_median']:.1f} s",
                  "baseline of the planned airflow sign"))
    if a["ptc_max_heating_c"] is not None:
        T.append(("PTC maximum while heating", f"{a['ptc_max_heating_c']:.1f} C",
                  "check against PB_HEATER_PTC_FOLDBACK_CUT_82K_C / _33K_C (pb_heater.h)"))
    s = res["step"]
    if s.get("found"):
        f = s["fits"].get("chamber_c")
        if f and f.get("tuning"):
            t = f["tuning"]["simc_smooth"]
            T.append(("PID gains (SIMC, smooth)", f"Kp {t['kp']:.4f}, Ki {t['ki']:.6f}, Kd 0",
                      "PB_HEATER_PID_KP / _KI / _KD, components/pb_heater/include/pb_heater_pid.h"))
        if s.get("max_rise_rate_c_per_s") is not None:
            T.append(("max chamber rise rate", f"{s['max_rise_rate_c_per_s']:.3f} C/s",
                      "compare with PB_HEATER_PID_APPROACH_RATE_C_PER_S (0.06)"))
        if s.get("overshoot_c") is not None:
            T.append(("bang-bang overshoot", f"{s['overshoot_c']:.1f} C",
                      "margin for PB_HEATER_ABS_MAX_TARGET_C (70) below the 85 C trip"))
    return T


def render(res, cap, prior_args):
    L = [f"# Capture analysis: {os.path.basename(os.path.abspath(cap.path))}", ""]
    sm = res["summary"]
    L.append(f"- {sm['samples']} samples over {sm['duration_s'] / 60:.1f} min "
             f"(median step {fmt(sm['median_dt_s'], '.2f')} s), {sm['metrics_snapshots']} /metrics "
             f"snapshots. Source: {sm['kind']}.")
    L.append(f"- Firmware {sm['firmware']}, Rref {sm['rref_kohm']} k, scenario {sm['scenario']}, "
             f"method {sm['method']}." + (f" SYNTHETIC: {sm['synthetic']}." if sm["synthetic"] else ""))
    if sm["notes"]:
        L.append(f"- Notes: {sm['notes']}")
    if sm["columns_missing"]:
        L.append(f"- Columns not in this capture: {', '.join(sm['columns_missing'])}.")
    L += ["", "## Flags", ""]
    L += [f"- {lvl}: {txt}" for lvl, txt in res["flags"]] or ["- none"]
    L += ["", "## Numbers for the firmware", "", "| Quantity | Value | Where it goes |", "|---|---|---|"]
    L += [f"| {a} | {b} | {c} |" for a, b, c in numbers_table(res)]
    L.append("")
    c = res["cold"]
    idle = (c["chamber_median_c"], c["ptc_median_c"]) if c.get("found") else None
    L += priors.render(prior_args.get("ambient_c") if prior_args.get("ambient_c") is not None
                       else cap.header.get("ambient_c"), cap.rref_kohm or 82, idle,
                       prior_args.get("mains_v"), prior_args.get("rated_v"))
    L += ["", "## First heat (measured)", ""]
    fh = res["first_heat"]
    if not fh.get("found"):
        L.append(f"Not found: {fh['why']}.")
    else:
        L.append(f"First relay ON at t = {fh['t_on_s']:.1f} s after {fh['off_before_s']:.0f} s OFF. "
                 f"Start: PTC {fmt(fh['ptc_start_c'])} C, chamber {fmt(fh['chamber_start_c'])} C "
                 f"({'cold' if fh['cold'] else 'NOT cold: PTC and chamber differ by 5 C or more'}). "
                 f"PTC noise (1 sigma, 60 s before): {fmt(fh['ptc_noise_sigma_c'], '.2f')} C.")
        L += ["", "| ON time | Wall time | PTC rise | Chamber rise | PTC/chamber | Rule at this point |",
              "|---|---|---|---|---|---|"]
        for sec in (12, 30):
            x = fh.get(f"at_{sec}s")
            if x:
                L.append(f"| {sec} s | {x['wall_s']:.1f} s | {x['ptc_rise_c']:.1f} C | "
                         f"{x['ch_rise_c']:.1f} C | {fmt(x['ratio_ptc_ch'])} | {x['rule']} |")
            else:
                L.append(f"| {sec} s | not reached | | | | |")
        m = fh["model"]
        L.append(f"\nThe provisional rule (PASS: PTC rise >= 3 C and > 2 x chamber rise; CROSSED: "
                 f"chamber rise >= 3 C and > PTC rise; NO_RISE at 30 s ON) on the whole window "
                 f"gives **{m['model_state'].upper()}**"
                 + (f" after {m['decided_at_on_s']:.1f} s ON" if m["decided_at_on_s"] else "")
                 + (f". The firmware reported {m['firmware_state']}." if m["firmware_state"] else "."))
        if fh.get("suggest"):
            sg = fh["suggest"]
            L.append(f"\nSuggested: PB_FH_PASS_RISE_DC = {sg['PB_FH_PASS_RISE_DC']}, "
                     f"PB_FH_CROSS_RISE_DC = {sg['PB_FH_CROSS_RISE_DC']}. {sg['rule']} "
                     f"Margins: PASS {fmt(sg['pass_margin_x'], '.1f')} x, "
                     f"CROSSED {fmt(sg['cross_margin_x'], '.1f')} x. One capture is one board: "
                     "take the smallest value over all first-heat captures.")
            if sg.get("warning"):
                L.append(f"\nWARNING: {sg['warning']}.")
    L += ["", "## Cold agreement (measured)", ""]
    if not c.get("found"):
        L.append(f"Not found: {c['why']}.")
    else:
        L.append(f"{c['samples']} idle samples over {c['hours']:.2f} h. PTC - chamber: median "
                 f"{c['diff_median_c']:+.2f} C, p5 {c['diff_p5_c']:+.2f}, p95 {c['diff_p95_c']:+.2f}, "
                 f"std {c['diff_std_c']:.2f}, drift {fmt(c['diff_slope_c_per_h'], '+.2f')} C/h. "
                 f"Peak-to-peak: chamber {c['chamber_pp_c']:.1f} C, PTC {c['ptc_pp_c']:.1f} C.")
        o = c["with_other_rref"]
        L.append(f"\nMedians: chamber {c['chamber_median_c']:.1f} C, PTC {c['ptc_median_c']:.1f} C. "
                 f"With Rref {o['rref_kohm']} k the same pin voltages read chamber {o['chamber_c']:.1f} C, "
                 f"PTC {o['ptc_c']:.1f} C. Reference: "
                 + (f"{c['reference_c']:.1f} C ({c['reference_source']})." if c["reference_c"] is not None
                    else "none (give --ambient-c)."))
        L.append(f"\nResult: {'agree' if c['agree'] else 'DISAGREE'} (limit {COLD_LIMIT_C:.0f} C)."
                 + (" The idle PTC oscillates while the chamber is steady." if c["oscillation"] else ""))
        for name, ch in c["chain"]["channels"].items():
            if ch["ratio"] is not None and abs(ch["ratio"] - 1) > 0.1:
                L.append(f"\nHypotheses for {name} (resistance ratio {ch['ratio']:.2f}):")
                L += [f"- {f}" for f in ch["fits"]]
        for h in c["rref_hint"]:
            L.append(f"\nHint: {h}")
    a = res["airflow"]
    L += ["", "## Airflow (measured)", ""]
    if a["heat_starts"]:
        L.append(f"Outlet (chamber NTC) rise / PTC rise after {AIRFLOW_WINDOW_S:.0f} s ON at each heat "
                 f"start (OFF for {HEAT_START_OFF_S:.0f} s or more before):")
        L += ["", "| t | PTC rise | Chamber rise | Ratio |", "|---|---|---|---|"]
        for s in a["heat_starts"][:20]:
            L.append(f"| {s['t_s']:.0f} s | {s['ptc_rise_c']:.1f} C | {s['ch_rise_c']:.1f} C | "
                     f"{fmt(s['ratio'], '.3f')} |")
        L.append(f"\nMedian ratio {fmt(a['ratio_median'], '.3f')}. A stalled fan gives a ratio near 0 "
                 "and a faster PTC rise.")
    else:
        L.append("No heat start with 60 s OFF before it and 30 s ON in 90 s.")
    L.append(f"\nFoldback: {a['foldback_cycles']} cycles in {a['heating_hours']:.2f} h of heat "
             f"({fmt(a['foldback_per_hour'])} per hour, source: {a['foldback_source']}, cut "
             f"{a['foldback_cut_c']:.0f} C). PTC maximum while the relay is on: "
             f"{fmt(a['ptc_max_heating_c'])} C.")
    L.append(f"\nPTC cooldown with the fan on (relay OFF after ON, first 120 s, exponential fit): "
             f"median tau {fmt(a['ptc_cool_tau_s_median'])} s over {a['ptc_cool_tau_count']} segments."
             + ("" if a["fan_column"] else " No fan column: the fan is assumed on."))
    s = res["step"]
    L += ["", "## Step response (measured)", ""]
    if not s.get("found"):
        L.append(f"Not found: {s['why']}.")
    else:
        L.append(f"Longest heat segment: target {s['target_c']:.1f} C for {s['duration_s'] / 60:.0f} min "
                 f"from t = {s['t_start_s']:.0f} s.")
        en = priors.enclosure()
        L.append(f"\nExpected (priors, printer chamber sensor): gain {en['gain_c_per_duty'][0]:.0f}-"
                 f"{en['gain_c_per_duty'][1]:.0f} C per duty, tau {en['tau_range'][0]:.0f}-"
                 f"{en['tau_range'][1]:.0f} min (median {en['tau_min']:.0f}). The 8-12 W/K loss is an "
                 "estimate. The local chamber NTC sits in the outlet: expect a larger gain and a fast part.")
        for col, f in s["fits"].items():
            L.append(f"\nFOPDT fit of {col} with the relay as input (0/1): K = {f['K_c_per_duty']:.1f} C "
                     f"per duty, tau = {f['tau_s'] / 60:.1f} min, dead time = {f['theta_s']:.0f} s, "
                     f"R^2 = {f['r2']:.3f} (grid {f['dt_s']:.1f} s)."
                     + ("" if f["tau_identifiable"] else " The segment is shorter than 3 tau: tau "
                        "and K are uncertain."))
            t = f.get("tuning")
            if t:
                L.append(f"Tuning with effective dead time {t['theta_eff_s']:.0f} s (fit + half the "
                         f"{PID_WINDOW_S:.0f} s SSR window + half a grid step). Firmware form: "
                         "duty = Kp e + Ki integral(e dt) + Kd d(-y)/dt.")
                L += ["", "| Method | Kp (1/C) | Ki (1/(C s)) | Kd (s/C) |", "|---|---|---|---|"]
                for name, label in (("simc_tight", "SIMC PI, tau_c = theta"),
                                    ("simc_smooth", "SIMC PI, tau_c = 3 theta (suggested)"),
                                    ("cohen_coon", "Cohen-Coon PID (reference only)")):
                    g = t[name]
                    L.append(f"| {label} | {g['kp']:.4f} | {g['ki']:.6f} | {g['kd']:.4f} |")
                L.append(f"| current firmware | {CURRENT_PID[0]:.4f} | {CURRENT_PID[1]:.6f} | "
                         f"{CURRENT_PID[2]:.4f} |")
        L.append(f"\nRecorded control: target reached after {fmt(s['reach_s'] and s['reach_s'] / 60)} min, "
                 f"overshoot {fmt(s.get('overshoot_c'))} C, inside +-{SETTLE_BAND_C} C after "
                 f"{fmt(s.get('settle_s') and s['settle_s'] / 60)} min"
                 + ("" if s.get("settled", True) else " (not settled)") + ".")
        if s.get("hold"):
            hd = s["hold"]
            L.append(f"Hold (second half after reach): mean {hd['mean_c']:.1f} C, peak-to-peak "
                     f"{hd['pp_c']:.1f} C, relay duty {fmt(hd['duty'], '.2f')}, "
                     f"{fmt(hd['relay_cycles_per_hour'], '.0f')} relay cycles per hour.")
        L.append(f"Maximum chamber rise rate (60 s window, before reach): "
                 f"{fmt(s['max_rise_rate_c_per_s'], '.3f')} C/s.")
    h = res["health"]
    L += ["", "## Control health (/metrics)", ""]
    if not h.get("found"):
        L.append(f"Not found: {h['why']}.")
    else:
        L.append(f"{h['snapshots']} snapshots. Late ticks +{fmt(h['late_ticks_delta'], '.0f')} of "
                 f"+{fmt(h['ticks_delta'], '.0f')}. Longest tick "
                 f"{fmt(h['tick_max_s'] and 1000 * h['tick_max_s'], '.2f')} ms, "
                 f"longest interval {fmt(h['interval_max_s'] and 1000 * h['interval_max_s'], '.0f')} ms. "
                 f"Heap minimum {fmt(h['heap_min_bytes'], '.0f')} B, smallest largest block "
                 f"{fmt(h['largest_block_min_bytes'], '.0f')} B. Reboots: {h['reboots']}.")
        if h["stack_min_bytes"]:
            L.append("Stack minima: " + ", ".join(f"{k} {v:.0f} B" for k, v in
                                                  sorted(h["stack_min_bytes"].items(), key=lambda x: x[1])) + ".")
    L.append("")
    L += replay_capture.render(res["_replay_full"])
    return L


def _jsonable(o):
    if isinstance(o, dict):
        return {k: _jsonable(v) for k, v in o.items() if not k.startswith("_")}
    if isinstance(o, (list, tuple)):
        return [_jsonable(v) for v in o]
    if isinstance(o, float) and not math.isfinite(o):
        return None
    return o


def make_expect(res):
    """Regression expectations: the verdicts exactly, the main numbers +-25 %."""
    checks = {"replay.ok": res["replay"]["ok"], "replay.violations": res["replay"]["violations"]}
    fh = res["first_heat"]
    if fh.get("found"):
        checks["first_heat.model.model_state"] = fh["model"]["model_state"]
    c = res["cold"]
    if c.get("found"):
        checks["cold.agree"] = c["agree"]

    def rng(v, abs_min=0.5):
        d = max(abs(v) * 0.25, abs_min)
        return [round(v - d, 3), round(v + d, 3)]
    a = res["airflow"]
    if a["ratio_median"] is not None:
        checks["airflow.ratio_median"] = rng(a["ratio_median"], 0.02)
    s = res["step"]
    if s.get("found") and s["fits"].get("chamber_c"):
        checks["step.fits.chamber_c.K_c_per_duty"] = rng(s["fits"]["chamber_c"]["K_c_per_duty"])
    return {"checks": checks}


def main(argv=None):
    p = argparse.ArgumentParser(description="Analyze a DragonBreath capture.")
    p.add_argument("capture", help="capture directory, state.csv, or a tools/diag.py CSV")
    p.add_argument("--ambient-c", type=float, help="room temperature from a thermometer")
    p.add_argument("--mains-v", type=float, help="mains voltage (for the priors)")
    p.add_argument("--rated-v", type=float, help="rated voltage on the unit label")
    p.add_argument("--quiet", action="store_true", help="do not print the report")
    p.add_argument("--write-expect", action="store_true", help="write expect.json for a replay test")
    a = p.parse_args(argv)
    cap = common.load_capture(a.capture)
    if not cap.rows:
        print("no samples in the capture")
        return 2
    res = analyze(cap, a.ambient_c)
    lines = render(res, cap, {"ambient_c": a.ambient_c, "mains_v": a.mains_v, "rated_v": a.rated_v})
    if os.path.isdir(a.capture):
        md, js = os.path.join(a.capture, "analysis.md"), os.path.join(a.capture, "analysis.json")
    else:
        md, js = a.capture + ".analysis.md", a.capture + ".analysis.json"
    with open(md, "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(js, "w") as f:
        json.dump(_jsonable(res), f, indent=1, sort_keys=True)
    if a.write_expect:
        ep = os.path.join(cap.out_dir, "expect.json")
        with open(ep, "w") as f:
            json.dump(make_expect(res), f, indent=1, sort_keys=True)
        print(f"wrote {ep}")
    if not a.quiet:
        print("\n".join(lines))
    print(f"\nwrote {md} and {js}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
