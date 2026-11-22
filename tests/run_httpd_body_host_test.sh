#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-httpd-body-test"

# shellcheck disable=SC2086
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${HOST_TEST_CFLAGS:-} \
  -I"$root/components/pb_httpd/include" \
  "$root/tests/pb_httpd_body_host_test.c" \
  -o "$out"

"$out"
