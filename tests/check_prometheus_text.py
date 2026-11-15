"""Parse Prometheus text (format 0.0.4) with prometheus-client.

Usage: python check_prometheus_text.py FILE

Fails if the text does not parse, if a family has no type, or if a series
(name and labels) occurs twice.
"""
import sys

from prometheus_client.parser import text_string_to_metric_families


def main() -> int:
    with open(sys.argv[1], encoding="utf-8") as f:
        text = f.read()
    seen = set()
    families = 0
    samples = 0
    for family in text_string_to_metric_families(text):
        families += 1
        if family.type == "unknown":
            print(f"family without TYPE: {family.name}", file=sys.stderr)
            return 1
        for s in family.samples:
            key = (s.name, tuple(sorted(s.labels.items())))
            if key in seen:
                print(f"duplicate series: {key}", file=sys.stderr)
                return 1
            seen.add(key)
            samples += 1
    print(f"prometheus-client parse: {families} families, {samples} samples: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
