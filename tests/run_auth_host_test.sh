#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pb-auth-test"

# shellcheck disable=SC2086 # HOST_TEST_CFLAGS is a list of flags
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${HOST_TEST_CFLAGS:-} \
  -I"$root/components/pb_httpd/include" \
  "$root/components/pb_httpd/pb_auth.c" \
  "$root/tests/pb_auth_host_test.c" \
  -o "$out"

"$out"
