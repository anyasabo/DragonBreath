#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Static checks on the firmware ELF.

The script disassembles the ELF with objdump and builds a call graph. It then
does two checks:

1. Hot-path calls. It lists each function that a root (default: control_task)
   can reach and that calls logging, printf, soft-double math, a blocking
   FreeRTOS/NVS/flash call or malloc. In CI mode (--ci), it fails if an entry is
   not in the allowlist.
2. Worst-case stack. If the build used -fstack-usage (cmake -DDB_STACK_USAGE=ON),
   it adds the frame sizes along the deepest path from each task entry.
   Recursion, indirect calls and functions with no .su data make the result a
   lower bound. The report marks these as "unknown".

Limits of a static graph:
- It cannot see how often a call runs. A call that runs once each 2 s and a
  call that runs each tick look the same.
- It does not follow indirect calls (function pointers, callbacks).
- It cannot see into ROM code. A call to a ROM function is a leaf.

Usage:
  python3 tools/elf_check.py build-release/dragonbreath.elf
  python3 tools/elf_check.py --ci --allowlist tools/elf_check_allowlist.txt ELF
  python3 tools/elf_check.py --stack --build-dir build-release ELF
"""

import argparse
import collections
import glob
import os
import re
import shutil
import subprocess
import sys

# Categories of calls that must not be on the control path. The check stops at
# these functions: it does not follow the calls inside them.
CATEGORIES = [
    ("log", re.compile(r"^(esp_log_write\w*|esp_log_writev|esp_rom_printf|ets_printf)$")),
    ("printf", re.compile(
        r"^(v?[sf]?n?printf|v?asn?printf|puts|putchar|fputs|fwrite|"
        r"_v?[sf]?n?printf_r|_svfn?printf_r|_vfn?printf_r|_svfiprintf_r|_vfiprintf_r|"
        r"__ssputs_r|__sfputs_r|_printf_float|_printf_i)$")),
    ("double", re.compile(
        r"^(__(add|sub|mul|div|neg)df3|__extendsfdf2|__truncdfsf2|"
        r"__fix(uns)?dfsi|__fix(uns)?dfdi|__float(un)?sidf|__float(un)?didf|"
        r"__(eq|ne|lt|le|gt|ge|unord)df2)$")),
    ("blocking", re.compile(
        r"^(vTaskDelay|vTaskDelayUntil|xTaskDelayUntil|xQueueReceive|xQueuePeek|"
        r"xEventGroupWaitBits|xStreamBufferReceive|xStreamBufferSend|"
        r"ulTaskGenericNotifyTake|xTaskGenericNotifyWait|"
        r"nvs_\w+|esp_flash_\w+|spi_flash_\w+|esp_partition_(read|write|erase)\w*|"
        r"esp_vfs_\w+|uart_write_bytes|uart_wait_tx_done|"
        r"lwip_(send|sendto|write|recv|recvfrom|read|connect|select)\w*|"
        r"esp_transport_(write|read|connect)\w*|esp_tls_conn_(write|read)|"
        r"mbedtls_ssl_(write|read|handshake))$")),
    # A mutex or queue call that can wait. A static check cannot see the timeout.
    # Most of these are short mutex sections (give: timeout 0).
    ("lock", re.compile(
        r"^(xQueueSemaphoreTake|xQueueGenericSend|xQueueTakeMutexRecursive|"
        r"xQueueGiveMutexRecursive|lock_acquire_generic)$")),
    ("malloc", re.compile(
        r"^(malloc|calloc|realloc|free|_malloc_r|_calloc_r|_realloc_r|_free_r|"
        r"heap_caps_(malloc|calloc|realloc|aligned_alloc|free)\w*|pvPortMalloc|vPortFree|strdup|strndup)$")),
]

FUNC_RE = re.compile(r"^([0-9a-f]+) <([^>]+)>:$")
# "jal 42032eea <esp_task_wdt_add>", "j 42008c50 <x>", "jalr 976(ra) # 40387db0 <x>",
# "jr -1056(t1) # 42001422 <panic_handler>"
CALL_RE = re.compile(r"^\s*[0-9a-f]+:\s+(jal|j|jalr|jr)\s+(.*)$")
TARGET_RE = re.compile(r"<([^>+]+)>\s*$")
INDIRECT_RE = re.compile(r"^(ra,\s*)?[a-z][a-z0-9]*(,\s*0)?$|^0\([a-z][a-z0-9]*\)$")


def find_tool(name, explicit):
    if explicit:
        return explicit
    path = shutil.which("riscv32-esp-elf-" + name)
    if path:
        return path
    deps = os.environ.get("DEVSPACE_DEPS", "")
    if deps:
        hits = sorted(glob.glob(os.path.join(
            deps, "esp-idf/*/tools/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-" + name)))
        if hits:
            return hits[-1]
    sys.exit("elf_check: riscv32-esp-elf-%s not found (use --objdump)" % name)


class Graph:
    """Direct calls of each function, and the functions with indirect calls."""

    def __init__(self):
        self.calls = collections.defaultdict(collections.Counter)  # caller -> callee -> count
        self.indirect = collections.Counter()                         # caller -> count

    @classmethod
    def from_objdump(cls, lines):
        g = cls()
        cur = None
        for line in lines:
            m = FUNC_RE.match(line)
            if m:
                cur = m.group(2)
                continue
            if cur is None:
                continue
            m = CALL_RE.match(line)
            if not m:
                continue
            op, arg = m.group(1), m.group(2).strip()
            t = TARGET_RE.search(arg)
            if t:
                target = t.group(1)
                # j/jr to a label inside the same function is a branch, not a call.
                if "+0x" in arg.split("<", 1)[1] or target == cur:
                    continue
                g.calls[cur][target] += 1
            elif op == "jalr":
                g.indirect[cur] += 1
            # A plain "jr REG" is a return or a jump table. Ignore it.
        return g

    def known(self, name):
        return name in self.calls or name in self.indirect


def categorize(name):
    for cat, rx in CATEGORIES:
        if rx.match(name):
            return cat
    return None


def reach(graph, roots):
    """Breadth-first walk. Returns {function: parent} for each reachable function.
    The walk does not go into a categorized function."""
    parent = {}
    queue = collections.deque()
    for r in roots:
        if r not in parent:
            parent[r] = None
            queue.append(r)
    while queue:
        f = queue.popleft()
        for callee in graph.calls.get(f, ()):
            if callee in parent or categorize(callee):
                continue
            parent[callee] = f
            queue.append(callee)
    return parent


def path_to(parent, f):
    out = []
    while f is not None:
        out.append(f)
        f = parent[f]
    return list(reversed(out))


def hot_path_entries(graph, roots):
    """Returns a list of (caller, callee, category, count, path)."""
    parent = reach(graph, roots)
    entries = []
    for caller in parent:
        for callee, n in graph.calls.get(caller, {}).items():
            cat = categorize(callee)
            if cat:
                entries.append((caller, callee, cat, n, path_to(parent, caller)))
    entries.sort(key=lambda e: (e[2], e[0], e[1]))
    return entries, parent


def read_allowlist(path):
    allowed = set()
    if not path or not os.path.exists(path):
        return allowed
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 2:
                sys.exit("elf_check: bad allowlist line: %r" % line)
            allowed.add((parts[0], parts[1]))
    return allowed


# ---- stack usage -----------------------------------------------------------

SU_RE = re.compile(r"^(.*):(\d+):(\d+):(\S+)\s+(\d+)\s+(\S+)$")


def read_stack_usage(build_dir):
    """Reads the .su files of a build. Returns {function: (bytes, qualifier)}.
    If two static functions have one name, it keeps the larger frame."""
    frames = {}
    for su in glob.glob(os.path.join(build_dir, "**", "*.su"), recursive=True):
        with open(su, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = SU_RE.match(line.strip())
                if not m:
                    continue
                name, size, qual = m.group(4), int(m.group(5)), m.group(6)
                if name not in frames or frames[name][0] < size:
                    frames[name] = (size, qual)
    return frames


def worst_stack(graph, frames, root):
    """Deepest path from root. Returns (bytes, path, notes). notes is a set of
    (reason, function) for the reachable functions that make the number a lower
    bound: recursion, an indirect call, a dynamic frame or no .su data (ROM,
    prebuilt libraries)."""
    memo = {}
    notes_all = set()
    def visit(f, stack):
        if f in memo:
            return memo[f]
        if f in stack:
            notes_all.add(("recursion", f))
            return (0, [f + " (recursion)"])
        frame = frames.get(f)
        own = frame[0] if frame else 0
        if frame is None:
            notes_all.add(("no .su", f))
        elif frame[1] != "static":
            notes_all.add((frame[1] + " frame", f))
        if graph.indirect.get(f):
            notes_all.add(("indirect call", f))
        stack.add(f)
        best = (0, [])
        for callee in graph.calls.get(f, ()):
            sub = visit(callee, stack)
            if sub[0] > best[0]:
                best = sub
        stack.discard(f)
        res = (own + best[0], [f] + best[1])
        memo[f] = res
        return res

    total, path = visit(root, set())
    return total, path, notes_all


TASK_RE = re.compile(
    r"xTaskCreate(?:PinnedToCore|Static|StaticPinnedToCore)?\s*\(\s*&?(\w+)\s*,\s*([^,]+),\s*([^,]+),")


def find_tasks(src_dirs):
    """Finds xTaskCreate*(entry, name, stack, ...) in the sources."""
    tasks = []
    for d in src_dirs:
        for path in glob.glob(os.path.join(d, "**", "*.c"), recursive=True):
            if "/examples/" in path or "/test" in path:
                continue
            with open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
            for m in TASK_RE.finditer(text):
                tasks.append((m.group(1), m.group(2).strip(), m.group(3).strip(), path))
    return tasks


# ---- main ------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("--objdump", help="objdump binary (default: riscv32-esp-elf-objdump)")
    ap.add_argument("--root", action="append", help="root function (default: control_task)")
    ap.add_argument("--allowlist", help="file with 'caller callee' lines")
    ap.add_argument("--ci", action="store_true", help="exit 1 on an entry not in the allowlist")
    ap.add_argument("--write-allowlist", metavar="FILE", help="write the current entries as an allowlist")
    ap.add_argument("--stack", action="store_true", help="add the worst-case stack of each task")
    ap.add_argument("--build-dir", help="build dir with the .su files (default: the ELF dir)")
    ap.add_argument("--src", action="append", help="source dirs to find tasks (default: main components managed_components)")
    ap.add_argument("--task", action="append", default=[], help="more task entry functions")
    ap.add_argument("--paths", action="store_true", help="show the call path to each entry")
    args = ap.parse_args(argv)

    objdump = find_tool("objdump", args.objdump)
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", args.elf],
                         check=True, capture_output=True, text=True).stdout
    graph = Graph.from_objdump(out.splitlines())
    roots = args.root or ["control_task"]
    for r in roots:
        if not graph.known(r):
            sys.exit("elf_check: root %s is not in the ELF" % r)

    entries, reached = hot_path_entries(graph, roots)
    print("# Hot-path calls reachable from %s (%d functions reached)" % (", ".join(roots), len(reached)))
    by_cat = collections.Counter(e[2] for e in entries)
    print("# entries: %d (%s)" % (len(entries), ", ".join("%s %d" % kv for kv in sorted(by_cat.items()))))
    allowed = read_allowlist(args.allowlist)
    new = []
    for caller, callee, cat, n, path in entries:
        mark = ""
        if args.allowlist:
            if (caller, callee) in allowed:
                mark = "  [allowed]"
            else:
                mark = "  [NEW]"
                new.append((caller, callee))
        print("%-9s %-40s -> %-24s x%-3d%s" % (cat, caller, callee, n, mark))
        if args.paths:
            print("          via " + " > ".join(path))
    indirect = sorted((f, graph.indirect[f]) for f in reached if graph.indirect.get(f))
    if indirect:
        print("# Reached functions with indirect calls (not followed): %d" % len(indirect))
        for f, n in indirect:
            print("indirect  %-40s x%d" % (f, n))
    stale = sorted(allowed - {(e[0], e[1]) for e in entries})
    for caller, callee in stale:
        print("# allowlist entry not found any more (remove it): %s %s" % (caller, callee))

    if args.write_allowlist:
        with open(args.write_allowlist, "w", encoding="utf-8") as fh:
            fh.write("# Hot-path calls that the review accepted. Format: CALLER CALLEE.\n")
            fh.write("# tools/elf_check.py --ci fails on a call that is not in this file.\n")
            for caller, callee, cat, _n, _p in entries:
                fh.write("%s %s  # %s\n" % (caller, callee, cat))

    if args.stack:
        bdir = args.build_dir or os.path.dirname(os.path.abspath(args.elf))
        frames = read_stack_usage(bdir)
        print()
        if not frames:
            print("# Worst-case stack: no .su files in %s. Build with -DDB_STACK_USAGE=ON." % bdir)
        else:
            src = args.src or [d for d in ("main", "components", "managed_components") if os.path.isdir(d)]
            tasks = [(t[0], t[1], t[2]) for t in find_tasks(src)] + [(t, "?", "?") for t in args.task]
            print("# Worst-case stack from -fstack-usage (%d functions with data)" % len(frames))
            print("# 'lower bound' = the path has recursion, indirect calls or code with no .su data")
            seen = set()
            for entry, name, size in tasks:
                if entry in seen:
                    continue
                seen.add(entry)
                if not graph.known(entry):
                    print("stack     %-28s %-18s not in ELF" % (entry, name))
                    continue
                total, path, notes = worst_stack(graph, frames, entry)
                bound = "lower bound" if notes else "exact"
                kinds = collections.Counter(k for k, _f in notes)
                print("stack     %-28s %-18.18s %6d B of %-8.8s %s (%s)" % (
                    entry, name, total, size, bound,
                    ", ".join("%s %d" % kv for kv in sorted(kinds.items())) or "-"))
                if args.paths:
                    print("          via " + " > ".join(path))

    if args.ci and new:
        print("\nelf_check: %d hot-path call(s) not in %s:" % (len(new), args.allowlist), file=sys.stderr)
        for caller, callee in new:
            print("  %s %s" % (caller, callee), file=sys.stderr)
        print("Remove the call from the control path, or add it to the allowlist with a reason.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
