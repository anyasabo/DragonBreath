#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay a DragonBreath capture through the heater decision MODEL and check it.

  python3 tools/replay_capture.py CAPTURE                    # report, exit 1 on a violation
  python3 tools/replay_capture.py CAPTURE --expect FILE      # also check analysis values
  python3 tools/replay_capture.py CAPTURE --export-adc FILE  # input for a C host replay

Checks:
  safety invariants   the relay is never on while a fault is latched, while not armed,
                      with PTC >= 105 C, chamber >= 85 C, or a sensor not ok. A violation
                      must hold for two samples (the state API averages 5 samples).
  decisions           for the bang-bang method: the relay state of each sample against
                      tools/heater_model.py. Samples near a threshold or a transition are
                      "ambiguous", all others that disagree are "unexplained".
  first heat          the provisional first-heat rule on the first relay ON.

The model is NOT the firmware (see heater_model.py). The C path: after the host
environment of test/heater-control-tick is merged, a C host test can feed the
--export-adc file (pin millivolts per 0.5 s tick, from tools/ntc_model.py) into
the real pb_ntc.c and pb_heater.c. docs/CAPTURES.md describes this.
Standard library only.
"""
import argparse
import csv
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dbcapture_common as common  # noqa: E402
import heater_model as hm  # noqa: E402
import ntc_model as ntc  # noqa: E402

AMBIG_TEMP_C = 0.6       # near a bang-bang threshold
AMBIG_PTC_C = 1.5        # near a foldback threshold
MAX_UNEXPLAINED = 0.01   # share of compared samples


def _armed(r):
    if r.get("target_c") is not None:
        return r["target_c"] > 0
    return None


def check_invariants(cap):
    """Return a list of violations: dicts with rule, t_s, samples."""
    rules = {
        "relay on while a fault is latched": lambda r: r.get("fault") == 1 or r.get("inhibited") == 1,
        "relay on while not armed": lambda r: _armed(r) is False or r.get("mode") == "off",
        "relay on with PTC >= 105 C": lambda r: r.get("ptc_c") is not None and r["ptc_c"] >= hm.PTC_CUTOFF_C,
        "relay on with chamber >= 85 C": lambda r: (r.get("chamber_c") is not None
                                                     and r["chamber_c"] >= hm.CHAMBER_MAX_C),
        "relay on with a sensor not ok": lambda r: any(
            r.get(k) not in (None, "ok") for k in ("chamber_status", "ptc_status")),
    }
    out = []
    for name, bad in rules.items():
        run, start = 0, None
        for r in cap.rows:
            if r.get("relay") == 1 and bad(r):
                run += 1
                start = r["t_s"] if run == 1 else start
                if run == 2:
                    out.append({"rule": name, "t_s": start})
            else:
                run = 0
    return out


def replay_decisions(cap, rref=None, fb_cut=None):
    rref = rref or cap.rref_kohm or 82
    if fb_cut is None:
        fb_cut = float((cap.header.get("settings") or {}).get("fb_cut") or 0)
    method = (cap.header.get("heater_method") or "")
    methods = {r.get("method") for r in cap.rows if r.get("method")}
    if "pid" in methods or method == "pid":
        return {"modeled": False, "why": "method pid: only the safety invariants are checked"}
    if not cap.has("target_c") or not cap.has("relay"):
        return {"modeled": False, "why": "no target or relay column (for example a diag.py CSV)"}
    m = hm.BangBangModel(rref_kohm=rref, fb_cut_c=fb_cut)
    cut, resume = m.cut_c, m.resume_c
    compared = mismatch = ambiguous = 0
    first_unexplained = []
    rows = cap.rows
    for i, r in enumerate(rows):
        ch, ptc, tgt, rec = r.get("chamber_c"), r.get("ptc_c"), r.get("target_c"), r.get("relay")
        if ch is None or ptc is None or tgt is None or rec is None:
            continue
        if r.get("fault") == 1:
            m.latched = True
        elif m.latched and r.get("fault") == 0:
            m.clear()
        drive, _ = m.step(ch, ptc, tgt)
        compared += 1
        if int(drive) == rec:
            continue
        mismatch += 1
        near = (abs(ch - tgt) <= AMBIG_TEMP_C or abs(ch - (tgt - hm.HYSTERESIS_C)) <= AMBIG_TEMP_C
                or abs(ptc - cut) <= AMBIG_PTC_C or abs(ptc - resume) <= AMBIG_PTC_C)
        edge = any(0 <= j < len(rows) and rows[j].get("relay") not in (None, rec)
                   for j in (i - 2, i - 1, i + 1, i + 2))
        prev_tgt = rows[i - 1].get("target_c") if i else tgt
        if near or edge or prev_tgt != tgt:
            ambiguous += 1
        elif len(first_unexplained) < 10:
            first_unexplained.append(r["t_s"])
        # resync the model latches to the recorded relay, so one disagreement does not cascade
        if rec:
            m.heat_intent, m.fb_cut = True, False
        elif ptc >= resume:
            m.fb_cut = True
        else:
            m.heat_intent = False
    unexplained = mismatch - ambiguous
    return {"modeled": True, "rref_kohm": rref, "fb_cut_c": cut, "fb_resume_c": resume,
            "compared": compared, "mismatches": mismatch, "ambiguous": ambiguous,
            "unexplained": unexplained,
            "unexplained_share": unexplained / compared if compared else 0.0,
            "first_unexplained_t_s": first_unexplained}


def replay_first_heat(cap):
    fh = hm.FirstHeatModel()
    for r in cap.rows:
        if r.get("relay") is None:
            continue
        fh.feed(r["t_s"], r["relay"], r.get("ptc_c"), r.get("chamber_c"))
    fw = [r.get("fh_state") for r in cap.rows if r.get("fh_state")]
    return {"model_state": fh.state, "decided_at_on_s": (None if fh.decided_at_on_ms is None
                                                         else fh.decided_at_on_ms / 1000.0),
            "ptc_rise_c": fh.ptc_rise_c, "ch_rise_c": fh.ch_rise_c,
            "firmware_state": fw[-1] if fw else None}


def replay(cap):
    inv = check_invariants(cap)
    dec = replay_decisions(cap)
    fh = replay_first_heat(cap)
    ok = not inv and (not dec.get("modeled") or dec["unexplained_share"] <= MAX_UNEXPLAINED)
    if fh["firmware_state"] and fh["model_state"] not in ("idle", "armed", "eval"):
        ok = ok and fh["firmware_state"] == fh["model_state"]
    return {"ok": ok, "violations": inv, "decisions": dec, "first_heat": fh}


def export_adc(cap, path, tick_s=0.5):
    """Write t_ms,chamber_mv,ptc_mv,target_c for a C host replay: one line each tick.

    The temperatures are the capture values (5-sample averages). The pin voltage is the
    one that gives this temperature with the board Rref. Gaps hold the last value."""
    rref = cap.rref_kohm or 82
    rows = [r for r in cap.rows if r.get("chamber_c") is not None and r.get("ptc_c") is not None]
    if not rows:
        raise ValueError("no samples with both temperatures")
    t, i, n = rows[0]["t_s"], 0, 0
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(("t_ms", "chamber_mv", "ptc_mv", "target_c", "relay_recorded"))
        while t <= rows[-1]["t_s"]:
            while i + 1 < len(rows) and rows[i + 1]["t_s"] <= t:
                i += 1
            r = rows[i]
            w.writerow((int(round((t - rows[0]["t_s"]) * 1000)),
                        int(round(ntc.temp_to_mv(r["chamber_c"], rref))),
                        int(round(ntc.temp_to_mv(r["ptc_c"], rref))),
                        "" if r.get("target_c") is None else f"{r['target_c']:.1f}",
                        "" if r.get("relay") is None else r["relay"]))
            t += tick_s
            n += 1
    return n


def _flatten(d, prefix=""):
    out = {}
    for k, v in d.items():
        key = f"{prefix}{k}"
        if isinstance(v, dict):
            out.update(_flatten(v, key + "."))
        else:
            out[key] = v
    return out


def check_expect(values, expect):
    """expect: {"checks": {"dotted.key": value | [lo, hi]}}. Return a list of failures."""
    flat = _flatten(values)
    fails = []
    for key, want in expect.get("checks", {}).items():
        got = flat.get(key)
        if isinstance(want, list) and len(want) == 2 and all(
                isinstance(x, (int, float)) for x in want):
            if not isinstance(got, (int, float)) or not want[0] <= got <= want[1]:
                fails.append(f"{key}: {got} not in [{want[0]}, {want[1]}]")
        elif got != want:
            fails.append(f"{key}: {got!r} != {want!r}")
    return fails


def render(res):
    L = ["## Replay (model, not firmware)", ""]
    if res["violations"]:
        for v in res["violations"]:
            L.append(f"- VIOLATION at t={v['t_s']:.1f} s: {v['rule']}")
    else:
        L.append("- Safety invariants: no violation.")
    d = res["decisions"]
    if d.get("modeled"):
        L.append(f"- Bang-bang decisions: {d['compared']} samples, {d['mismatches']} disagree, "
                 f"{d['ambiguous']} of them near a threshold or a transition, "
                 f"{d['unexplained']} unexplained ({100 * d['unexplained_share']:.2f} %, "
                 f"limit {100 * MAX_UNEXPLAINED:.0f} %).")
        if d["first_unexplained_t_s"]:
            L.append("  First unexplained at t = " + ", ".join(f"{t:.1f}" for t in
                                                               d["first_unexplained_t_s"]) + " s.")
    else:
        L.append(f"- Decisions not replayed: {d['why']}.")
    f = res["first_heat"]
    L.append(f"- First-heat model: {f['model_state']}"
             + (f" after {f['decided_at_on_s']:.1f} s ON" if f["decided_at_on_s"] else "")
             + (f", firmware reported {f['firmware_state']}" if f["firmware_state"] else "")
             + ".")
    L.append(f"- Result: {'PASS' if res['ok'] else 'FAIL'}")
    return L


def main(argv=None):
    p = argparse.ArgumentParser(description="Replay a capture through the heater model.")
    p.add_argument("capture")
    p.add_argument("--expect", help="expect.json to check against the analysis values")
    p.add_argument("--export-adc", metavar="FILE", help="write the C host replay input")
    a = p.parse_args(argv)
    cap = common.load_capture(a.capture)
    res = replay(cap)
    print("\n".join(render(res)))
    ok = res["ok"]
    if a.export_adc:
        n = export_adc(cap, a.export_adc)
        print(f"wrote {n} ticks to {a.export_adc}")
    if a.expect:
        import analyze_capture
        with open(a.expect) as f:
            expect = json.load(f)
        fails = check_expect(analyze_capture.analyze(cap), expect)
        for x in fails:
            print("EXPECT FAIL", x)
        ok = ok and not fails
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
