# SPDX-License-Identifier: MIT
"""Unit tests for tools/elf_check.py and tools/ram_treemap.py (no toolchain needed)."""
import os
import sys
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

42000200 <helper>:
42000200:\tjal\t42000100 <tick>
42000204:\tjal\t42000500 <snprintf>
42000208:\tjal\t42000500 <snprintf>
4200020c:\tret

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

    def test_entries_stop_at_categorized_functions(self):
        entries, reached = elf_check.hot_path_entries(self.g, ["control_task"])
        pairs = {(e[0], e[1]): e[2] for e in entries}
        self.assertEqual(pairs, {
            ("control_task", "ulTaskGenericNotifyTake"): "blocking",
            ("tick", "__extendsfdf2"): "double",
            ("tick", "esp_log_write"): "log",
            ("helper", "snprintf"): "printf",
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
