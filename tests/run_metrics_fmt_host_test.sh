#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pb-metrics-fmt-test"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${HOST_TEST_CFLAGS:-} \
  -I"$root/components/pb_metrics/include" \
  "$root/components/pb_metrics/pb_metrics_fmt.c" \
  "$root/tests/pb_metrics_fmt_host_test.c" \
  -o "$out"

"$out"
