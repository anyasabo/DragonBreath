"""Unit tests for helpers in tools/flash.py (no hardware, no esptool)."""

import importlib.util
import os
import random
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("dragonbreath_flash", ROOT / "tools" / "flash.py")
flash = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(flash)


class EsptoolVersion(unittest.TestCase):
    def test_esptool5_output(self):
        self.assertEqual(flash.esptool_version("esptool v5.6.0\n5.6.0\n"), (5, "5.6.0"))

    def test_esptool4_output(self):
        self.assertEqual(flash.esptool_version("esptool.py v4.8.1\n4.8.1\n"), (4, "4.8.1"))

    def test_prerelease(self):
        self.assertEqual(flash.esptool_version("esptool v5.0.0rc1\n5.0.0rc1\n")[0], 5)

    def test_warning_on_last_line(self):
        out = "esptool v5.6.0\n5.6.0\nWARNING: something\n"
        self.assertEqual(flash.esptool_version(out), (5, "5.6.0"))

    def test_no_version(self):
        self.assertEqual(flash.esptool_version("No module named esptool\n"), (None, None))


class ScriptCommand(unittest.TestCase):
    def test_single_file(self):
        self.assertEqual(flash.script_command("flash.py"), "uv run flash.py")

    def test_repo_path(self):
        self.assertEqual(flash.script_command("tools/flash.py"), "uv run tools/flash.py")

    def test_path_with_space(self):
        self.assertEqual(flash.script_command("C:\\My Files\\flash.py"),
                         "uv run \"C:\\My Files\\flash.py\"")



class ChunkedRead(unittest.TestCase):
    """read_flash_chunked() with a fake esptool that drops some reads."""

    SIZE = 0x80000      # 512 KB fake flash: 4 chunks of 128 KB
    CHUNK = 0x20000

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.image = random.Random(1).randbytes(self.SIZE)
        self.calls = []
        self.fail = {}          # offset -> number of reads of that chunk that fail

    def fake_run(self, cmd):
        self.calls.append(cmd)
        args = cmd[cmd.index("read-flash") + 1:]
        offset, length, out = int(args[0], 16), int(args[1], 16), args[2]
        if self.fail.get(offset, 0) > 0:
            self.fail[offset] -= 1
            with open(out, "wb") as f:      # esptool can leave a part
                f.write(b"\0" * 10)
            return 2
        with open(out, "wb") as f:
            f.write(self.image[offset:offset + length])
        return 0

    def read(self, bauds=(460800, 230400, 115200)):
        path = os.path.join(self.tmp.name, "backup.bin")
        with mock.patch.object(flash, "run", self.fake_run):
            ok = flash.read_flash_chunked(None, list(bauds), path,
                                          size=self.SIZE, chunk=self.CHUNK)
        return ok, path

    def test_clean_read(self):
        ok, path = self.read()
        self.assertTrue(ok)
        self.assertEqual(Path(path).read_bytes(), self.image)
        self.assertEqual(len(self.calls), self.SIZE // self.CHUNK)
        # The app must not boot between chunks: it can write NVS.
        for c in self.calls:
            self.assertEqual(c[c.index("--after") + 1], "no-reset")

    def test_retries_only_the_failed_chunk(self):
        self.fail = {0x20000: 2, 0x60000: 1}
        ok, path = self.read()
        self.assertTrue(ok)
        self.assertEqual(Path(path).read_bytes(), self.image)
        self.assertEqual(len(self.calls), self.SIZE // self.CHUNK + 3)
        # 2 failures stay at the first baud rate
        self.assertTrue(all("460800" in c for c in self.calls))

    def test_steps_down_the_baud_ladder(self):
        self.fail = {0x40000: flash.CHUNK_TRIES_PER_BAUD}
        ok, path = self.read()
        self.assertTrue(ok)
        self.assertEqual(Path(path).read_bytes(), self.image)
        self.assertIn("230400", self.calls[-2])     # chunk 3 at the next rate

    def test_gives_up_and_removes_the_file(self):
        self.fail = {0x20000: 999}
        ok, path = self.read()
        self.assertFalse(ok)
        self.assertFalse(os.path.exists(path))
        self.assertFalse(os.path.exists(path + ".part"))

    def test_short_chunk_is_a_failure(self):
        real = self.fake_run
        done = []

        def short_once(cmd):
            rc = real(cmd)
            if not done and cmd[cmd.index("read-flash") + 1] == "0x0":
                done.append(1)
                out = cmd[cmd.index("read-flash") + 3]
                with open(out, "r+b") as f:
                    f.truncate(100)
            return rc

        path = os.path.join(self.tmp.name, "backup.bin")
        with mock.patch.object(flash, "run", short_once):
            ok = flash.read_flash_chunked(None, [460800], path,
                                          size=self.SIZE, chunk=self.CHUNK)
        self.assertTrue(ok)
        self.assertEqual(Path(path).read_bytes(), self.image)

if __name__ == "__main__":
    unittest.main()
