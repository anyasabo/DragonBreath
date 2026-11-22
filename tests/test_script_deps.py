"""Each script in tools/ declares its third-party dependencies (PEP 723).

Then `uv run tools/NAME.py` installs them, and the block tells a plain
`python3` user what to install. A script that imports a package, or runs
`python -m PACKAGE`, must list the distribution in its `# /// script` block.
"""

import ast
import re
import sys
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"

# Import name -> distribution name, where they differ.
DISTRIBUTION = {"serial": "pyserial"}

# The reference regex of PEP 723.
BLOCK = re.compile(
    r"(?m)^# /// (?P<type>[a-zA-Z0-9-]+)$\s(?P<content>(^#(| .*)$\s)+)^# ///$")


def script_metadata(source):
    """Return the parsed `script` block, or None if there is no block."""
    blocks = [m for m in BLOCK.finditer(source) if m.group("type") == "script"]
    if len(blocks) > 1:
        raise ValueError("more than one script block")
    if not blocks:
        return None
    content = "".join(
        line[2:] if line.startswith("# ") else line[1:]
        for line in blocks[0].group("content").splitlines(keepends=True))
    return tomllib.loads(content)


def requirement_name(requirement):
    name = re.match(r"[A-Za-z0-9._-]+", requirement).group(0)
    return re.sub(r"[-_.]+", "-", name).lower()


def needed_distributions(source):
    """Third-party distributions that the script imports or runs with -m."""
    local = {p.stem for p in TOOLS.glob("*.py")}
    modules = set()
    tree = ast.parse(source)
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            modules |= {a.name.split(".")[0] for a in node.names}
        elif isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
            modules.add(node.module.split(".")[0])
        elif isinstance(node, (ast.List, ast.Tuple)):
            # [sys.executable, "-m", "esptool", ...]
            values = [e.value if isinstance(e, ast.Constant) else None for e in node.elts]
            for i, v in enumerate(values[:-1]):
                if v == "-m" and isinstance(values[i + 1], str):
                    modules.add(values[i + 1].split(".")[0])
    third_party = {m for m in modules
                   if m not in sys.stdlib_module_names and m not in local}
    return {requirement_name(DISTRIBUTION.get(m, m)) for m in third_party}


class ScriptMetadataParse(unittest.TestCase):
    def test_parses_block(self):
        source = ('#!/usr/bin/env python3\n'
                  '# /// script\n'
                  '# requires-python = ">=3.10"\n'
                  '# dependencies = ["esptool>=5", "py_serial"]\n'
                  '# ///\n')
        meta = script_metadata(source)
        self.assertEqual(meta["requires-python"], ">=3.10")
        self.assertEqual({requirement_name(r) for r in meta["dependencies"]},
                         {"esptool", "py-serial"})

    def test_finds_dash_m(self):
        source = 'import sys\ncmd = [sys.executable, "-m", "esptool", "version"]\n'
        self.assertEqual(needed_distributions(source), {"esptool"})

    def test_maps_import_name(self):
        self.assertEqual(needed_distributions("import serial\n"), {"pyserial"})

    def test_ignores_stdlib_and_local(self):
        self.assertEqual(needed_distributions("import json\nimport diag\n"), set())


class ToolsDeclareDependencies(unittest.TestCase):
    def test_each_tool(self):
        scripts = sorted(TOOLS.glob("*.py"))
        self.assertTrue(scripts)
        for path in scripts:
            with self.subTest(script=path.name):
                source = path.read_text(encoding="utf-8")
                needed = needed_distributions(source)
                meta = script_metadata(source)
                if not needed:
                    continue
                self.assertIsNotNone(
                    meta, f"{path.name} needs {sorted(needed)} but has no "
                          "'# /// script' block")
                declared = {requirement_name(r) for r in meta.get("dependencies", [])}
                self.assertLessEqual(needed, declared,
                                     f"{path.name}: not declared: {sorted(needed - declared)}")
                self.assertIn("requires-python", meta)


if __name__ == "__main__":
    unittest.main()
