# SPDX-License-Identifier: MIT
"""Shared code of the DragonBreath capture tools: the capture format and its loader.

A capture is one directory:

    header.json        firmware, board Rref, settings, host time, scenario, notes
    state.csv          one row for each GET /api/v2/state (columns: STATE_COLUMNS)
    state.jsonl.gz     the raw JSON of each state (gzip members, append-only)
    metrics/index.csv  t_s,file of each GET /metrics snapshot
    metrics/*.prom     the raw Prometheus text
    events.csv         t_s,kind,text: prompts, commands, errors, reboots

load_capture() also reads a bare state.csv and the CSV of tools/diag.py.
Standard library only.
"""
import csv
import gzip
import json
import math
import os

FORMAT = "dragonbreath-capture"
FORMAT_VERSION = 1

STATE_COLUMNS = (
    "t_s", "unix_s", "chamber_c", "chamber_status", "ptc_c", "ptc_status",
    "relay", "demand", "duty", "constraint", "method", "mode", "source",
    "target_c", "fan_pct", "fan_reason", "fault", "inhibited", "reason",
    "revision", "boot_id", "printer_chamber_c",
    "fh_state", "fh_ptc_rise_c", "fh_ch_rise_c",
)
FLOAT_COLS = {"t_s", "unix_s", "chamber_c", "ptc_c", "duty", "target_c", "fan_pct",
              "printer_chamber_c", "fh_ptc_rise_c", "fh_ch_rise_c"}
BOOL_COLS = {"relay", "demand", "fault", "inhibited"}
INT_COLS = {"revision"}
HEAT_MODES = ("power_on", "auto", "drying")


def _get(d, *path):
    for p in path:
        if not isinstance(d, dict):
            return None
        d = d.get(p)
    return d


def _bool01(v):
    return "" if v is None else int(bool(v))


def _num(v):
    return "" if v is None or isinstance(v, bool) else v


def flatten_state(st, t_s, unix_s):
    """One state.csv row from a GET /api/v2/state answer. Missing fields stay empty."""
    fh = _get(st, "first_heat") or {}
    row = {
        "t_s": f"{t_s:.2f}", "unix_s": f"{unix_s:.3f}",
        "chamber_c": _num(_get(st, "sensors", "chamber", "temperature_c")),
        "chamber_status": _get(st, "sensors", "chamber", "status") or "",
        "ptc_c": _num(_get(st, "sensors", "ptc", "temperature_c")),
        "ptc_status": _get(st, "sensors", "ptc", "status") or "",
        "relay": _bool01(_get(st, "heater", "output")),
        "demand": _bool01(_get(st, "heater", "demand")),
        "duty": _num(_get(st, "heater", "commanded_duty")),
        "constraint": _get(st, "heater", "constraint") or "",
        "method": _get(st, "heater", "method") or "",
        "mode": _get(st, "mode") or "",
        "source": _get(st, "source") or "",
        "target_c": _num(_get(st, "target", "effective_c")),
        "fan_pct": _num(_get(st, "fan", "effective_percent")),
        "fan_reason": _get(st, "fan", "reason") or "",
        "fault": _bool01(_get(st, "safety", "fault_latched")),
        "inhibited": _bool01(_get(st, "safety", "inhibited")),
        "reason": _get(st, "safety", "reason") or "",
        "revision": _num(_get(st, "state_revision")),
        "boot_id": _get(st, "boot_id") or "",
        "printer_chamber_c": _num(_get(st, "environment", "printer_chamber_temperature_c")),
        "fh_state": fh.get("state") or "" if isinstance(fh, dict) else "",
        "fh_ptc_rise_c": _num(fh.get("ptc_rise_c")) if isinstance(fh, dict) else "",
        "fh_ch_rise_c": _num(fh.get("ch_rise_c")) if isinstance(fh, dict) else "",
    }
    return row


def parse_metrics(text):
    """Prometheus text 0.0.4 to {name: [(labels dict, value)]}. Bad lines are skipped."""
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        try:
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
        except (ValueError, IndexError):
            continue
    return out


def metric(m, name, **labels):
    """First sample of a family that matches the labels, or None."""
    for lab, v in m.get(name, ()):
        if all(lab.get(k) == val for k, val in labels.items()):
            return v
    return None


def _typed(col, s):
    if s is None:
        return None
    s = s.strip()
    if s == "":
        return None
    if col in FLOAT_COLS:
        try:
            v = float(s)
        except ValueError:
            return None
        return v if math.isfinite(v) else None
    if col in BOOL_COLS:
        return 1 if s.lower() in ("1", "true", "yes") else 0
    if col in INT_COLS:
        try:
            return int(float(s))
        except ValueError:
            return None
    return s


class Capture:
    """A loaded capture. rows: list of dicts with STATE_COLUMNS keys (None if not known)."""

    def __init__(self, path, header, rows, metrics, events, kind):
        self.path, self.header, self.rows = path, header, rows
        self.metrics, self.events, self.kind = metrics, events, kind

    def col(self, name):
        return [r.get(name) for r in self.rows]

    def has(self, name):
        return any(r.get(name) is not None for r in self.rows)

    @property
    def rref_kohm(self):
        v = _get(self.header, "info", "rref_kohm") or self.header.get("rref_kohm")
        try:
            return int(v) if v else None
        except (TypeError, ValueError):
            return None

    @property
    def out_dir(self):
        return self.path if os.path.isdir(self.path) else os.path.dirname(os.path.abspath(self.path))


def _read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def _rows_from_diag(raw):
    """tools/diag.py columns: t_s,chamber_c,ptc_c,ptc_status,out,mode,fault,reason,peak_c."""
    rows = []
    for r in raw:
        row = {c: None for c in STATE_COLUMNS}
        row.update({
            "t_s": _typed("t_s", r.get("t_s")),
            "chamber_c": _typed("chamber_c", r.get("chamber_c")),
            "ptc_c": _typed("ptc_c", r.get("ptc_c")),
            "ptc_status": _typed("ptc_status", r.get("ptc_status")),
            "relay": _typed("relay", r.get("out")),
            "mode": _typed("mode", r.get("mode")),
            "fault": _typed("fault", r.get("fault")),
            "reason": _typed("reason", r.get("reason")),
        })
        if row["ptc_status"] and row["ptc_status"] != "ok":
            row["ptc_c"] = None
        rows.append(row)
    return rows


def load_capture(path):
    """Load a capture directory, a state.csv, or a tools/diag.py CSV."""
    header, metrics, events = {}, [], []
    if os.path.isdir(path):
        d = path
        csv_path = os.path.join(d, "state.csv")
        hp = os.path.join(d, "header.json")
        if os.path.exists(hp):
            with open(hp) as f:
                header = json.load(f)
        idx = os.path.join(d, "metrics", "index.csv")
        if os.path.exists(idx):
            for r in _read_csv(idx):
                fp = os.path.join(d, "metrics", r["file"])
                try:
                    with open(fp) as f:
                        metrics.append((float(r["t_s"]), parse_metrics(f.read())))
                except (OSError, ValueError):
                    continue
        ep = os.path.join(d, "events.csv")
        if os.path.exists(ep):
            for r in _read_csv(ep):
                try:
                    events.append((float(r["t_s"]), r.get("kind", ""), r.get("text", "")))
                except (KeyError, ValueError):
                    continue
    else:
        csv_path = path
        hp = os.path.join(os.path.dirname(os.path.abspath(path)), "header.json")
        if os.path.exists(hp):
            with open(hp) as f:
                header = json.load(f)
    raw = _read_csv(csv_path)
    cols = set(raw[0].keys()) if raw else set()
    if "out" in cols and "peak_c" in cols:
        rows, kind = _rows_from_diag(raw), "diag"
    else:
        rows = [{c: _typed(c, r.get(c)) for c in STATE_COLUMNS} for r in raw]
        kind = "capture"
    rows = [r for r in rows if r["t_s"] is not None]
    rows.sort(key=lambda r: r["t_s"])
    return Capture(path, header, rows, metrics, events, kind)


class CaptureWriter:
    """Writes a capture directory. Each write is flushed, so an interrupted capture stays usable."""

    def __init__(self, out_dir, header):
        self.dir = out_dir
        os.makedirs(os.path.join(out_dir, "metrics"), exist_ok=True)
        self.header = dict(header, format=FORMAT, format_version=FORMAT_VERSION, complete=False)
        self.write_header()
        self._state = open(os.path.join(out_dir, "state.csv"), "w", newline="")
        self._sw = csv.DictWriter(self._state, fieldnames=STATE_COLUMNS)
        self._sw.writeheader()
        self._events = open(os.path.join(out_dir, "events.csv"), "w", newline="")
        self._ew = csv.writer(self._events)
        self._ew.writerow(("t_s", "kind", "text"))
        self._midx = open(os.path.join(out_dir, "metrics", "index.csv"), "w", newline="")
        self._mw = csv.writer(self._midx)
        self._mw.writerow(("t_s", "file"))
        self._raw = []
        self.samples = 0
        self.metrics_count = 0

    def write_header(self):
        tmp = os.path.join(self.dir, "header.json.tmp")
        with open(tmp, "w") as f:
            json.dump(self.header, f, indent=2, sort_keys=True)
            f.write("\n")
        os.replace(tmp, os.path.join(self.dir, "header.json"))

    def state(self, row, raw_state):
        self._sw.writerow(row)
        self._state.flush()
        self._raw.append(json.dumps({"t_s": float(row["t_s"]), "state": raw_state},
                                    separators=(",", ":")))
        self.samples += 1
        if len(self._raw) >= 20:
            self.flush_raw()

    def flush_raw(self):
        if not self._raw:
            return
        # Each flush is one complete gzip member. Concatenated members are valid gzip.
        with gzip.open(os.path.join(self.dir, "state.jsonl.gz"), "ab") as g:
            g.write(("\n".join(self._raw) + "\n").encode())
        self._raw = []

    def metrics(self, t_s, text):
        name = f"m{self.metrics_count:05d}.prom"
        with open(os.path.join(self.dir, "metrics", name), "w") as f:
            f.write(text)
        self._mw.writerow((f"{t_s:.2f}", name))
        self._midx.flush()
        self.metrics_count += 1

    def event(self, t_s, kind, text):
        self._ew.writerow((f"{t_s:.2f}", kind, text))
        self._events.flush()

    def close(self, **end):
        self.flush_raw()
        for f in (self._state, self._events, self._midx):
            f.close()
        self.header.update(end, complete=True, samples=self.samples,
                           metrics_snapshots=self.metrics_count)
        self.write_header()
