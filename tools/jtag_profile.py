#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Sample the stack of an ESP32-C3 over JTAG and write folded stacks.

The script connects to the Tcl port of a running OpenOCD (default 6666). Each
N ms, it halts the CPU, reads pc, ra and s0, walks the frame-pointer chain in
target memory, and resumes the CPU. At the end, it converts the addresses to
function names with addr2line and writes one line for each unique stack:

    pb_control_task;pb_policy_tick;pb_heater_tick;pb_ntc_read 42

flamegraph.pl and speedscope read this "folded" format.

The firmware must have frame pointers (sdkconfig.profile-devboard, ESP-IDF
v6.1). Without them, s0 is a general register and the walk stops at once: the
stacks then have the pc and the ra only.

Each sample halts the CPU for a few milliseconds. The sample shows the code
that runs at the halt: the idle task is the free CPU time. The halts change
the timing of the firmware. Use the result to find hot code, not to measure
exact times.

Usage (OpenOCD runs in another terminal, see docs/PROFILING.md):
  python3 tools/jtag_profile.py --elf build/dragonbreath.elf --interval-ms 20 \\
      --samples 2000 -o profile.folded
  flamegraph.pl profile.folded > profile.svg
"""

import argparse
import collections
import glob
import os
import re
import shutil
import socket
import subprocess
import sys
import time

TERMINATOR = b"\x1a"
MAX_DEPTH = 32


class OpenOcd:
    """A minimal client of the OpenOCD Tcl RPC port."""

    def __init__(self, host="127.0.0.1", port=6666, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    def cmd(self, command):
        self.sock.sendall(command.encode() + TERMINATOR)
        while TERMINATOR not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("OpenOCD closed the connection")
            self.buf += chunk
        reply, _, self.buf = self.buf.partition(TERMINATOR)
        return reply.decode(errors="replace").strip()

    def reg(self, name):
        # "pc (/32): 0x42001234"
        reply = self.cmd("reg " + name)
        m = re.search(r"0x([0-9a-fA-F]+)\s*$", reply)
        if not m:
            raise ValueError("cannot read register %s: %r" % (name, reply))
        return int(m.group(1), 16)

    def read_words(self, addr, count):
        reply = self.cmd("read_memory 0x%x 32 %d" % (addr, count))
        words = [int(w, 16) for w in reply.split()]
        if len(words) != count:
            raise ValueError("cannot read memory at 0x%x: %r" % (addr, reply))
        return words

    def close(self):
        self.sock.close()


def is_data(addr):
    """ESP32-C3 internal data RAM (the stacks are there)."""
    return 0x3FC80000 <= addr < 0x3FCE0000 and addr % 4 == 0


def walk(pc, ra, fp, read_words, max_depth=MAX_DEPTH):
    """Returns the raw addresses of one sample, innermost first: pc, ra, and
    the return addresses of the frame-pointer chain (ra at fp-4, the previous
    fp at fp-8, as ESP-IDF esp_fp_get_callers)."""
    chain = []
    seen = set()
    while is_data(fp) and len(chain) < max_depth and fp not in seen:
        seen.add(fp)
        try:
            prev_fp, ret = read_words(fp - 8, 2)
        except (ValueError, OSError):
            break
        if ret == 0:
            break
        chain.append(ret)
        fp = prev_fp
    return [pc, ra] + chain


def fold(frames, symbolize):
    """Addresses (innermost first) to one folded line (outermost first). The
    ra is kept only if its function is not the pc function and not the first
    chain function: in a function with a frame, the ra register can be old."""
    names = [symbolize(a) for a in frames]
    pc_name, ra_name, chain = names[0], names[1], names[2:]
    stack = [pc_name]
    if ra_name not in (pc_name, chain[0] if chain else None) and ra_name != "??":
        stack.append(ra_name)
    stack.extend(chain)
    # A return address can point to the next function after a noreturn call.
    out = []
    for n in stack:
        if not out or out[-1] != n:
            out.append(n)
    return ";".join(reversed(out))


def find_addr2line(explicit):
    if explicit:
        return explicit
    path = shutil.which("riscv32-esp-elf-addr2line")
    if path:
        return path
    deps = os.environ.get("DEVSPACE_DEPS", "")
    hits = sorted(glob.glob(os.path.join(
        deps, "esp-idf/*/tools/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-addr2line")))
    if hits:
        return hits[-1]
    sys.exit("jtag_profile: riscv32-esp-elf-addr2line not found (use --addr2line)")


def symbolizer(addr2line, elf, addresses):
    """One addr2line run for all unique addresses. A return address points
    after the call, so the script looks up addr - 2 (the call can be 2 bytes)."""
    addrs = sorted(set(addresses))
    names = {}
    if addrs:
        out = subprocess.run([addr2line, "-f", "-e", elf] + ["0x%x" % max(a - 2, 0) for a in addrs],
                             check=True, capture_output=True, text=True).stdout.splitlines()
        for i, a in enumerate(addrs):
            name = out[2 * i].strip() if 2 * i < len(out) else "??"
            names[a] = name if name else "??"
    return lambda a: names.get(a, "??")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", required=True, help="the ELF of the firmware on the board")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6666, help="OpenOCD Tcl port")
    ap.add_argument("--interval-ms", type=float, default=20.0)
    ap.add_argument("--samples", type=int, default=1000)
    ap.add_argument("--addr2line")
    ap.add_argument("-o", "--output", default="-", help="folded stacks (default: stdout)")
    args = ap.parse_args(argv)

    ocd = OpenOcd(args.host, args.port)
    raw = []
    errors = 0
    try:
        for i in range(args.samples):
            try:
                ocd.cmd("halt")
                pc, ra, fp = ocd.reg("pc"), ocd.reg("ra"), ocd.reg("s0")
                raw.append(walk(pc, ra, fp, ocd.read_words))
            except (ValueError, OSError) as e:
                errors += 1
                if errors > 20:
                    sys.exit("jtag_profile: too many errors, last: %s" % e)
            finally:
                ocd.cmd("resume")
            time.sleep(args.interval_ms / 1000.0)
            if (i + 1) % 100 == 0:
                print("jtag_profile: %d samples" % (i + 1), file=sys.stderr)
    except KeyboardInterrupt:
        pass
    finally:
        ocd.close()

    sym = symbolizer(find_addr2line(args.addr2line), args.elf, [a for s in raw for a in s])
    counts = collections.Counter(fold(s, sym) for s in raw)
    out = sys.stdout if args.output == "-" else open(args.output, "w", encoding="utf-8")
    for stack, n in counts.most_common():
        out.write("%s %d\n" % (stack, n))
    if out is not sys.stdout:
        out.close()
    print("jtag_profile: %d samples, %d stacks, %d errors" % (len(raw), len(counts), errors), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
