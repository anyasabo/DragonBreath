#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pb-metrics-fmt-test"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${HOST_TEST_CFLAGS:-} \
  -I"$root/components/pb_metrics/include" \
  "$root/components/pb_metrics/pb_metrics_fmt.c" \
  "$root/tests/pb_metrics_fmt_host_test.c" \
  -lm -o "$out"

"$out"

# Parse a realistic response with prometheus-client. It needs uv. CI sets
# METRICS_PARSE=require so that a missing uv fails the step.
if command -v uv >/dev/null 2>&1; then
  "$out" --dump > "$out.txt"
  uv run --quiet --no-project --with prometheus-client \
    python -I "$root/tests/check_prometheus_text.py" "$out.txt"
elif [ "${METRICS_PARSE:-}" = "require" ]; then
  echo "uv not found: cannot run the prometheus-client parse check" >&2
  exit 1
else
  echo "uv not found: prometheus-client parse check skipped"
fi
