#!/usr/bin/env python3
"""Check the licenses of the linked packages against tools/sbom/license-policy.json.

Input: the JSON of `esp-idf-sbom license --format json --packages`.
Exit 0 if each package passes, 1 if a package fails, 2 on a usage or input error.

Usage: python3 tools/sbom/license_check.py LICENSE_JSON [POLICY_JSON]
"""

import json
import pathlib
import re
import sys

TOKEN = re.compile(r"\(|\)|\bAND\b|\bOR\b|\bWITH\b|[A-Za-z0-9.+:-]+")


def parse(expr: str):
    """Parse an SPDX expression into nested ('AND'|'OR', [terms]) tuples and str leaves."""
    toks = TOKEN.findall(expr)
    pos = 0

    def peek():
        return toks[pos] if pos < len(toks) else None

    def take():
        nonlocal pos
        tok = peek()
        if tok is None:
            raise ValueError(f"unexpected end of {expr!r}")
        pos += 1
        return tok

    def atom():
        if peek() == "(":
            take()
            node = disj()
            if take() != ")":
                raise ValueError(f"missing ')' in {expr!r}")
            return node
        name = take()
        if name in ("AND", "OR", "WITH", ")", None):
            raise ValueError(f"unexpected {name!r} in {expr!r}")
        if peek() == "WITH":
            take()
            name = f"{name} WITH {take()}"
        return name

    def conj():
        terms = [atom()]
        while peek() == "AND":
            take()
            terms.append(atom())
        return terms[0] if len(terms) == 1 else ("AND", terms)

    def disj():
        terms = [conj()]
        while peek() == "OR":
            take()
            terms.append(conj())
        return terms[0] if len(terms) == 1 else ("OR", terms)

    node = disj()
    if pos != len(toks):
        raise ValueError(f"trailing tokens in {expr!r}")
    return node


def allowed(node, ok: set[str]) -> bool:
    if isinstance(node, str):
        return node in ok
    op, terms = node
    if op == "AND":
        return all(allowed(t, ok) for t in terms)
    return any(allowed(t, ok) for t in terms)


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(__doc__, file=sys.stderr)
        return 2
    report = json.loads(pathlib.Path(sys.argv[1]).read_text())
    policy_path = pathlib.Path(sys.argv[2]) if len(sys.argv) == 3 else pathlib.Path(__file__).with_name("license-policy.json")
    policy = json.loads(policy_path.read_text())
    ok = set(policy["allowed"])
    known = policy["packages"]

    failures = []
    used = set()
    for pkg in report["packages"]:
        name = pkg["name"]
        expr = pkg.get("license_concluded") or ""
        if not expr:
            if name not in known:
                failures.append(f"{name}: no license found and no entry in {policy_path.name}")
                continue
            expr = known[name]["license"]
            used.add(name)
        try:
            node = parse(expr)
        except ValueError as e:
            failures.append(f"{name}: {e}")
            continue
        if not allowed(node, ok):
            failures.append(f"{name}: license {expr!r} is not allowed")

    for name in sorted(set(known) - used):
        print(f"note: policy entry {name} was not used (the package has a license now, or is not linked)")
    for f in failures:
        print(f"error: {f}")
    print(f"license check: {len(report['packages'])} packages, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
