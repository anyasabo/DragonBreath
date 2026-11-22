"""Unit tests for helpers in tools/flash.py (no hardware, no esptool)."""

import importlib.util
import unittest
from pathlib import Path

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


if __name__ == "__main__":
    unittest.main()
