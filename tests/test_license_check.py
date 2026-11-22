"""Tests of tools/sbom/license_check.py: the SPDX expression parser and the policy check."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "sbom"))
import license_check as lc  # noqa: E402

OK = {"MIT", "Apache-2.0", "Apache-2.0 WITH LLVM-exception"}


class ExpressionTest(unittest.TestCase):
    def check(self, expr, want):
        self.assertEqual(lc.allowed(lc.parse(expr), OK), want, expr)

    def test_single(self):
        self.check("MIT", True)
        self.check("GPL-2.0-only", False)

    def test_or_takes_one_allowed_alternative(self):
        self.check("Apache-2.0 OR GPL-2.0-or-later", True)

    def test_and_needs_all_terms(self):
        self.check("MIT AND GPL-2.0-only", False)
        self.check("MIT AND Apache-2.0", True)

    def test_parentheses(self):
        self.check("(MIT OR GPL-3.0-only) AND Apache-2.0", True)
        self.check("(GPL-2.0-only OR LGPL-2.1-only) AND MIT", False)

    def test_with_is_one_license(self):
        self.check("Apache-2.0 WITH LLVM-exception", True)
        self.check("GPL-2.0-only WITH GCC-exception-2.0", False)

    def test_bad_expression(self):
        for bad in ("MIT AND", "(MIT", "OR MIT", "MIT)"):
            with self.assertRaises(ValueError, msg=bad):
                lc.parse(bad)


class PolicyTest(unittest.TestCase):
    def run_check(self, packages):
        policy = {"allowed": ["MIT"], "packages": {"known": {"license": "MIT", "source": "test"}}}
        with tempfile.TemporaryDirectory() as d:
            rep = pathlib.Path(d, "report.json")
            pol = pathlib.Path(d, "policy.json")
            rep.write_text(json.dumps({"packages": packages}))
            pol.write_text(json.dumps(policy))
            return subprocess.run([sys.executable, "-I", str(ROOT / "tools/sbom/license_check.py"), str(rep), str(pol)],
                                  capture_output=True, text=True)

    def test_pass(self):
        r = self.run_check([{"name": "a", "license_concluded": "MIT"}, {"name": "known", "license_concluded": ""}])
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_unknown_package_without_license_fails(self):
        r = self.run_check([{"name": "mystery", "license_concluded": ""}])
        self.assertEqual(r.returncode, 1)
        self.assertIn("mystery: no license found", r.stdout)

    def test_copyleft_fails(self):
        r = self.run_check([{"name": "a", "license_concluded": "GPL-3.0-only"}])
        self.assertEqual(r.returncode, 1)
        self.assertIn("not allowed", r.stdout)


if __name__ == "__main__":
    unittest.main()
