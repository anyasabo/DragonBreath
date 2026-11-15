#!/bin/sh
# Host test for the command replay cache (pb_httpd_replay.c).
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pb-httpd-replay-test"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${HOST_TEST_CFLAGS:-} \
  -I"$root/components/pb_httpd" \
  "$root/components/pb_httpd/pb_httpd_replay.c" \
  "$root/tests/pb_httpd_replay_host_test.c" \
  -o "$out"

"$out"
