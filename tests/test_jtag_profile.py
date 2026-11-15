# SPDX-License-Identifier: MIT
"""Unit tests for tools/jtag_profile.py with a fake OpenOCD Tcl server."""
import os
import socket
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import jtag_profile  # noqa: E402

# A stack in fake target memory: main_task -> outer -> inner (pc in inner).
# Frame of inner at 0x3fc90100: [fp-8] = outer frame, [fp-4] = return into outer.
MEM = {
    0x3fc900f8: 0x3fc90200, 0x3fc900fc: 0x42000210,   # inner frame
    0x3fc901f8: 0x3fc90300, 0x3fc901fc: 0x42000310,   # outer frame
    0x3fc902f8: 0x00000000, 0x3fc902fc: 0x00000000,   # main_task: end of chain
}
REGS = {"pc": 0x42000104, "ra": 0x42000210, "s0": 0x3fc90100}


def fake_symbol(a):
    if 0x42000100 <= a < 0x42000200:
        return "inner"
    if 0x42000200 <= a < 0x42000300:
        return "outer"
    if 0x42000300 <= a < 0x42000400:
        return "main_task"
    if 0x40000000 <= a < 0x40100000:
        return "rom_memcpy"
    return "??"


def read_words(addr, count):
    return [MEM.get(addr + 4 * i, 0) for i in range(count)]


class WalkTest(unittest.TestCase):
    def test_walk_and_fold(self):
        frames = jtag_profile.walk(REGS["pc"], REGS["ra"], REGS["s0"], read_words)
        self.assertEqual(frames, [0x42000104, 0x42000210, 0x42000210, 0x42000310])
        self.assertEqual(jtag_profile.fold(frames, fake_symbol), "main_task;outer;inner")

    def test_rom_leaf_keeps_ra(self):
        # pc in ROM (no frame pointer): s0 is still the frame of the caller "inner".
        frames = jtag_profile.walk(0x40000500, 0x42000108, 0x3fc90100, read_words)
        self.assertEqual(jtag_profile.fold(frames, fake_symbol), "main_task;outer;inner;rom_memcpy")

    def test_bad_fp_stops(self):
        self.assertEqual(jtag_profile.walk(1, 2, 0x12345678, read_words), [1, 2])
        self.assertEqual(jtag_profile.walk(1, 2, 0x3fc90101, read_words), [1, 2])   # unaligned
        loop = {0x3fc900f8: 0x3fc90100, 0x3fc900fc: 0x42000210}
        frames = jtag_profile.walk(1, 2, 0x3fc90100, lambda a, n: [loop.get(a + 4 * i, 0) for i in range(n)])
        self.assertEqual(frames, [1, 2, 0x42000210])                          # a loop stops

        def fail(_a, _n):
            raise ValueError("no memory")
        self.assertEqual(jtag_profile.walk(1, 2, 0x3fc90100, fail), [1, 2])

    def test_depth_limit(self):
        deep = {}
        for i in range(100):
            fp = 0x3fc90000 + 0x100 * i
            deep[fp - 8] = fp + 0x100
            deep[fp - 4] = 0x42000300
        frames = jtag_profile.walk(1, 2, 0x3fc90000, lambda a, n: [deep.get(a + 4 * i, 0) for i in range(n)])
        self.assertEqual(len(frames), 2 + jtag_profile.MAX_DEPTH)


class FakeOpenOcd(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.commands = []

    def run(self):
        conn, _ = self.srv.accept()
        buf = b""
        while True:
            data = conn.recv(4096)
            if not data:
                break
            buf += data
            while b"\x1a" in buf:
                cmd, _, buf = buf.partition(b"\x1a")
                cmd = cmd.decode()
                self.commands.append(cmd)
                reply = ""
                if cmd.startswith("reg "):
                    name = cmd.split()[1]
                    reply = "%s (/32): 0x%08x" % (name, REGS[name])
                elif cmd.startswith("read_memory "):
                    _, addr, _width, count = cmd.split()
                    reply = " ".join("0x%x" % w for w in read_words(int(addr, 16), int(count)))
                # Send the reply in two parts to test the framing.
                conn.sendall(reply.encode()[:3])
                conn.sendall(reply.encode()[3:] + b"\x1a")
        conn.close()


class OpenOcdClientTest(unittest.TestCase):
    def test_sample_over_tcl(self):
        srv = FakeOpenOcd()
        srv.start()
        ocd = jtag_profile.OpenOcd("127.0.0.1", srv.port)
        ocd.cmd("halt")
        frames = jtag_profile.walk(ocd.reg("pc"), ocd.reg("ra"), ocd.reg("s0"), ocd.read_words)
        ocd.cmd("resume")
        ocd.close()
        srv.join(2)
        self.assertEqual(jtag_profile.fold(frames, fake_symbol), "main_task;outer;inner")
        self.assertEqual(srv.commands[0], "halt")
        self.assertEqual(srv.commands[-1], "resume")
        self.assertIn("read_memory 0x3fc900f8 32 2", srv.commands)


if __name__ == "__main__":
    unittest.main()
