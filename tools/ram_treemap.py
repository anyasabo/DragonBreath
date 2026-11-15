#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""RAM treemap of the firmware from the linker map.

The script reads the input sections of the RAM output sections (.dram0.data,
.dram0.bss, .noinit, .iram0.*, .rtc.*) in build/dragonbreath.map. It writes:
- a text list of the largest symbols (stdout),
- an SVG treemap: output section > component (library) > symbol.

With -fdata-sections (the ESP-IDF default), each static variable has its own
input section, so a box is one variable.

Usage:
  python3 tools/ram_treemap.py build-release/dragonbreath.map -o ram.svg
  python3 tools/ram_treemap.py --top 40 build-release/dragonbreath.map
"""

import argparse
import collections
import html
import re
import sys

RAM_SECTIONS = re.compile(r"^\.(dram0\.(data|bss)|noinit|iram0\.(text|data|bss|vectors)|rtc[._]\w+)$")
OUT_RE = re.compile(r"^(\.[\w.]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)")
IN_FULL_RE = re.compile(r"^ (\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)$")
IN_NAME_RE = re.compile(r"^ (\S+)$")
IN_CONT_RE = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)$")
FILL_RE = re.compile(r"^ \*fill\*\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)")


def component(obj):
    """'esp-idf/pb_httpd/libpb_httpd.a(pb_httpd.c.obj)' -> 'pb_httpd'."""
    m = re.search(r"lib([\w+-]+)\.a\(", obj)
    if m:
        return m.group(1)
    m = re.search(r"([\w.+-]+)\.(o|obj)\)?$", obj)
    return m.group(1) if m else obj


def symbol(section_name):
    """'.bss.s_replay' -> 's_replay', '.dram1.3' -> '.dram1.3'."""
    for pfx in (".sbss.", ".bss.", ".sdata.", ".data.", ".srodata.", ".rodata.",
                ".iram1.", ".text.", ".rtc.bss.", ".rtc.data.", ".noinit."):
        rest = section_name[len(pfx):]
        if section_name.startswith(pfx) and rest and not rest.isdigit():
            return rest
    return section_name


def parse_map(lines):
    """Returns [(output_section, component, symbol, size)]."""
    items = []
    out = None
    pending = None
    started = False
    for line in lines:
        if not started:
            started = line.startswith("Linker script and memory map")
            continue
        m = OUT_RE.match(line)
        if m and not line.startswith(" "):
            out = m.group(1) if RAM_SECTIONS.match(m.group(1)) else None
            pending = None
            continue
        if line and not line.startswith(" "):
            # An output section name on its own line.
            name = line.strip()
            out = name if RAM_SECTIONS.match(name) else None
            pending = None
            continue
        if out is None:
            continue
        m = FILL_RE.match(line)
        if m:
            items.append((out, "(fill)", "(fill)", int(m.group(2), 16)))
            continue
        m = IN_FULL_RE.match(line)
        if m and not m.group(4).startswith("0x"):
            size = int(m.group(3), 16)
            if size:
                items.append((out, component(m.group(4)), symbol(m.group(1)), size))
            pending = None
            continue
        m = IN_NAME_RE.match(line)
        if m:
            pending = m.group(1)
            continue
        m = IN_CONT_RE.match(line)
        if m and pending:
            size = int(m.group(2), 16)
            if size:
                items.append((out, component(m.group(3)), symbol(pending), size))
            pending = None
    return items


# ---- squarified treemap ------------------------------------------------------

def squarify(values, x, y, w, h):
    """Bruls et al. Returns a rectangle (x, y, w, h) for each value (same order,
    values sorted largest first by the caller)."""
    rects = []
    total = float(sum(values))
    if total <= 0 or w <= 0 or h <= 0:
        return [(x, y, 0, 0)] * len(values)
    scale = w * h / total
    areas = [v * scale for v in values]
    i = 0
    while i < len(areas):
        short = min(w, h)
        row = [areas[i]]
        i += 1

        def worst(r):
            s = sum(r)
            return max(max(s * s / (short * short * a) if a else 0, short * short * a / (s * s)) for a in r)
        while i < len(areas) and worst(row + [areas[i]]) <= worst(row):
            row.append(areas[i])
            i += 1
        s = sum(row)
        if w >= h:   # lay the row out as a column on the left
            cw = s / h
            cy = y
            for a in row:
                rh = a / cw
                rects.append((x, cy, cw, rh))
                cy += rh
            x += cw
            w -= cw
        else:        # lay the row out as a row on the top
            rh = s / w
            cx = x
            for a in row:
                rw = a / rh
                rects.append((cx, y, rw, rh))
                cx += rw
            y += rh
            h -= rh
    return rects


PALETTE = ["#4e79a7", "#f28e2b", "#59a14f", "#e15759", "#76b7b2", "#edc948",
           "#b07aa1", "#ff9da7", "#9c755f", "#bab0ac"]


def svg_treemap(items, width, height, title):
    tree = collections.OrderedDict()
    for out, comp, sym, size in items:
        tree.setdefault(out, collections.Counter())
        tree[out][(comp, sym)] += size
    sec_sizes = sorted(((sum(c.values()), s) for s, c in tree.items()), reverse=True)
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" font-family="sans-serif" font-size="10">'
             % (width, height + 24),
             '<rect width="100%%" height="100%%" fill="#fff"/><text x="4" y="16" font-size="14">%s</text>'
             % html.escape(title)]
    sec_rects = squarify([s for s, _ in sec_sizes], 0, 24, width, height)
    color_of = {}
    for (ssize, sec), (sx, sy, sw, sh) in zip(sec_sizes, sec_rects):
        comps = collections.Counter()
        for (comp, _sym), size in tree[sec].items():
            comps[comp] += size
        comp_list = comps.most_common()
        comp_rects = squarify([v for _, v in comp_list], sx + 2, sy + 14, sw - 4, sh - 16)
        parts.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="#333"/>' % (sx, sy, sw, sh))
        parts.append('<text x="%.1f" y="%.1f" fill="#fff" font-weight="bold">%s %d B</text>'
                     % (sx + 3, sy + 11, html.escape(sec), ssize))
        for (comp, csize), (cx, cy, cw, ch) in zip(comp_list, comp_rects):
            col = color_of.setdefault(comp, PALETTE[len(color_of) % len(PALETTE)])
            syms = sorted(((v, s) for (c, s), v in tree[sec].items() if c == comp), reverse=True)
            srects = squarify([v for v, _ in syms], cx, cy, cw, ch)
            for (v, s), (rx, ry, rw, rh) in zip(syms, srects):
                parts.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s" stroke="#fff" '
                             'stroke-width="0.5"><title>%s / %s / %s: %d B</title></rect>'
                             % (rx, ry, rw, rh, col, html.escape(sec), html.escape(comp), html.escape(s), v))
                if rw > 60 and rh > 12:
                    parts.append('<text x="%.1f" y="%.1f" fill="#000">%s %d</text>'
                                 % (rx + 2, ry + 10, html.escape(s[:int(rw / 6)]), v))
            parts.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="none" stroke="#000"/>'
                         % (cx, cy, cw, ch))
            if cw > 50 and ch > 26:
                parts.append('<text x="%.1f" y="%.1f" font-weight="bold">%s %d</text>'
                             % (cx + 2, cy + ch - 3, html.escape(comp[:int(cw / 6)]), csize))
    parts.append("</svg>")
    return "\n".join(parts)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map")
    ap.add_argument("-o", "--svg", help="write the SVG treemap to this file")
    ap.add_argument("--top", type=int, default=25, help="number of symbols in the text list")
    ap.add_argument("--width", type=int, default=1400)
    ap.add_argument("--height", type=int, default=900)
    args = ap.parse_args(argv)
    with open(args.map, encoding="utf-8", errors="replace") as fh:
        items = parse_map(fh)
    if not items:
        sys.exit("ram_treemap: no RAM input sections in %s" % args.map)
    by_sec = collections.Counter()
    by_comp = collections.Counter()
    for out, comp, _sym, size in items:
        by_sec[out] += size
        if not out.startswith(".iram0"):
            by_comp[comp] += size
    print("# RAM by output section (bytes)")
    for sec, size in by_sec.most_common():
        print("%-20s %8d" % (sec, size))
    print("\n# Data RAM (.dram0, .noinit, .rtc) by component, top %d" % args.top)
    for comp, size in by_comp.most_common(args.top):
        print("%-28s %8d" % (comp, size))
    print("\n# Largest data symbols, top %d" % args.top)
    data = sorted((i for i in items if not i[0].startswith(".iram0") and i[2] != "(fill)"),
                  key=lambda i: -i[3])
    for out, comp, sym, size in data[:args.top]:
        print("%8d  %-14s %-22s %s" % (size, out, comp, sym))
    if args.svg:
        with open(args.svg, "w", encoding="utf-8") as fh:
            fh.write(svg_treemap(items, args.width, args.height, "RAM: " + args.map))
        print("\nwrote %s" % args.svg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
