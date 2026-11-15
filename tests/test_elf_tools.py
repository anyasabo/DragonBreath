# SPDX-License-Identifier: MIT
"""Unit tests for tools/elf_check.py and tools/ram_treemap.py (no toolchain needed)."""
import contextlib
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import elf_check  # noqa: E402
import ram_treemap  # noqa: E402

DIS = """
42000000 <control_task>:
42000000:\tjal\t42000100 <tick>
42000004:\tjalr\t976(ra) # 40387db0 <ulTaskGenericNotifyTake>
42000008:\tj\t42000000 <control_task+0x0>
4200000c:\tjalr\ta5

42000100 <tick>:
42000100:\tjal\t42000200 <helper>
42000104:\tjal\t42000300 <__extendsfdf2>
42000108:\tjr\t-1056(t1) # 42000400 <esp_log_write>
4200010c:\tjal\t42000800 <esp_log>

42000200 <helper>:
42000200:\tjal\t42000100 <tick>
42000204:\tjal\t42000500 <snprintf>
42000208:\tjal\t42000500 <snprintf>
4200020c:\tjr\ta5
42000210:\tret

42000300 <__extendsfdf2>:
42000300:\tjal\t42000600 <malloc>

42000500 <snprintf>:
42000500:\tjal\t42000600 <malloc>

42000700 <unrelated>:
42000700:\tjal\t42000600 <malloc>
""".splitlines()


class ElfCheckTest(unittest.TestCase):
    def setUp(self):
        self.g = elf_check.Graph.from_objdump(DIS)

    def test_graph(self):
        self.assertEqual(dict(self.g.calls["control_task"]), {"tick": 1, "ulTaskGenericNotifyTake": 1})
        self.assertEqual(self.g.indirect["control_task"], 1)
        self.assertEqual(self.g.calls["tick"]["esp_log_write"], 1)   # tail call through jr
        self.assertEqual(self.g.calls["helper"]["snprintf"], 2)
        self.assertEqual(self.g.indirect["helper"], 1)             # jr a5: indirect tail call

    def test_entries_stop_at_categorized_functions(self):
        entries, reached = elf_check.hot_path_entries(self.g, ["control_task"])
        pairs = {(e[0], e[1]): e[2] for e in entries}
        self.assertEqual(pairs, {
            ("control_task", "ulTaskGenericNotifyTake"): "blocking",
            ("tick", "__extendsfdf2"): "double",
            ("tick", "esp_log_write"): "log",
            ("tick", "esp_log"): "log",
            ("helper", "snprintf"): "printf",
            ("control_task", elf_check.INDIRECT): "indirect",
            ("helper", elf_check.INDIRECT): "indirect",
        })
        self.assertNotIn("unrelated", reached)
        self.assertNotIn("malloc", reached)

    def test_worst_stack_marks_recursion(self):
        frames = {"control_task": (32, "static"), "tick": (16, "static"), "helper": (64, "static")}
        total, path, notes = elf_check.worst_stack(self.g, frames, "control_task")
        self.assertEqual(total, 32 + 16 + 64)
        self.assertEqual(path[:3], ["control_task", "tick", "helper"])
        kinds = {k for k, _f in notes}
        self.assertIn("recursion", kinds)
        self.assertIn("indirect call", kinds)
        self.assertIn("no .su", kinds)


MAP = """Linker script and memory map

.iram0.text     0x40380000      0x20
 .iram1.5       0x40380000      0x10 esp-idf/freertos/libfreertos.a(tasks.c.obj)
 *fill*         0x40380010       0x4
 .iram1.6       0x40380014       0xc esp-idf/freertos/libfreertos.a(tasks.c.obj)

.dram0.bss      0x3fc95a50     0x2000
 .bss.s_replay  0x3fc96724     0x18d0 esp-idf/pb_httpd/libpb_httpd.a(pb_httpd.c.obj)
 .bss.a_very_long_symbol_name_that_wraps
                0x3fc98000      0x100 esp-idf/dc_evlog/libdc_evlog.a(dc_evlog.c.obj)
 .bss.empty     0x3fc98100        0x0 esp-idf/x/libx.a(x.c.obj)

.flash.text     0x42000000     0x1000
 .text.foo      0x42000000      0x100 esp-idf/x/libx.a(x.c.obj)
"""


class AllowlistCiTest(unittest.TestCase):
    FULL = ("control_task ulTaskGenericNotifyTake 1\ncontrol_task (indirect) 1\n"
            "tick __extendsfdf2 1\ntick esp_log_write 1\ntick esp_log 1\n"
            "helper snprintf 2\nhelper (indirect) 1\n")

    def run_ci(self, allow_text):
        with tempfile.TemporaryDirectory() as d:
            dis = os.path.join(d, "dis.txt")
            with open(dis, "w") as fh:
                fh.write("\n".join(DIS))
            fake = os.path.join(d, "objdump")
            with open(fake, "w") as fh:
                fh.write("#!/bin/sh\ncat %s\n" % dis)
            os.chmod(fake, 0o755)
            allow = os.path.join(d, "allow.txt")
            with open(allow, "w") as fh:
                fh.write(allow_text)
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                rc = elf_check.main(["--objdump", fake, "--ci", "--allowlist", allow, "x.elf"])
            return rc, err.getvalue()

    def test_all_allowed(self):
        self.assertEqual(self.run_ci(self.FULL)[0], 0)

    def test_more_call_sites_fail(self):
        rc, err = self.run_ci(self.FULL.replace("helper snprintf 2", "helper snprintf 1"))
        self.assertEqual(rc, 1)
        self.assertIn("helper snprintf 2 (allowed 1)", err)

    def test_new_indirect_site_fails(self):
        rc, err = self.run_ci(self.FULL.replace("helper (indirect) 1\n", ""))
        self.assertEqual(rc, 1)
        self.assertIn("helper (indirect)", err)

    def test_old_format_is_refused(self):
        with self.assertRaises(SystemExit):
            self.run_ci("tick esp_log_write\n")


class RamTreemapTest(unittest.TestCase):
    def test_parse(self):
        items = ram_treemap.parse_map(MAP.splitlines())
        self.assertIn((".dram0.bss", "pb_httpd", "s_replay", 0x18d0), items)
        self.assertIn((".dram0.bss", "dc_evlog", "a_very_long_symbol_name_that_wraps", 0x100), items)
        self.assertIn((".iram0.text", "freertos", ".iram1.5", 0x10), items)
        self.assertIn((".iram0.text", "(fill)", "(fill)", 4), items)
        self.assertFalse(any(i[0] == ".flash.text" for i in items))
        self.assertFalse(any(i[2] == "empty" for i in items))

    def test_squarify_keeps_area(self):
        rects = ram_treemap.squarify([6, 6, 4, 3, 2, 2, 1], 0, 0, 6, 4)
        self.assertAlmostEqual(sum(w * h for _x, _y, w, h in rects), 24.0)
        self.assertEqual(len(rects), 7)

    def test_svg(self):
        svg = ram_treemap.svg_treemap(ram_treemap.parse_map(MAP.splitlines()), 400, 300, "t")
        self.assertTrue(svg.startswith("<svg") and svg.endswith("</svg>"))
        self.assertIn("s_replay", svg)


if __name__ == "__main__":
    unittest.main()
