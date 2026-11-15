#!/bin/sh
# Host test for the status line (pb_status_line.c): text and repeat collapse.
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pb-status-line-test"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${HOST_TEST_CFLAGS:-} \
  -I"$root/components/pb_status/include" \
  "$root/components/pb_status/pb_status_line.c" \
  "$root/tests/pb_status_line_host_test.c" \
  -lm -o "$out"

"$out"
