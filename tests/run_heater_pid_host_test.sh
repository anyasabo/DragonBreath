#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
# Keep host coverage on the exact released dependency used by the firmware
# manifest (tests/fetch_dc_pid.sh reads the version from main/idf_component.yml).
dc_pid="$(sh "$root/tests/fetch_dc_pid.sh")"
out="$(mktemp "${TMPDIR:-/tmp}/dragonbreath-pb-heater-pid-test.XXXXXX")"
trap 'rm -f "$out"' EXIT

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${HOST_TEST_CFLAGS:-} \
  -I"$root/components/pb_heater/include" \
  -I"$root/tests/stubs" \
  -I"$dc_pid/include" \
  "$root/tests/pb_heater_pid_host_test.c" \
  "$dc_pid/dc_pid.c" \
  -lm -o "$out"

"$out"
