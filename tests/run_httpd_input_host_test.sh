#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-httpd-input-test"

# shellcheck disable=SC2086
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror ${HOST_TEST_CFLAGS:-} \
  -I"$root/tests/stubs" \
  -I"$root/components/pb_httpd" \
  -I"$root/components/pb_policy/include" \
  -I"$root/components/pb_buttons/include" \
  "$root/tests/pb_httpd_input_host_test.c" \
  -lm -o "$out"

"$out"
