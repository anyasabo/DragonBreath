#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
# Compile the REAL pb_heater.c with the stubs in tests/stubs_heater (first on
# the include path) and the dc_pid version that main/idf_component.yml pins.
dc_pid="$(sh "$root/tests/fetch_dc_pid.sh")"
out="$(mktemp "${TMPDIR:-/tmp}/dragonbreath-pb-heater-tick-test.XXXXXX")"
trap 'rm -f "$out"' EXIT

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${HOST_TEST_CFLAGS:-} \
  -I"$root/tests/stubs_heater" \
  -I"$root/components/pb_heater/include" \
  -I"$root/components/pb_ntc/include" \
  -I"$root/components/pb_board/include" \
  -I"$root/tests/stubs" \
  -I"$root/tests" \
  -I"$dc_pid/include" \
  "$root/components/pb_heater/pb_heater.c" \
  "$root/tests/heater_host_env.c" \
  "$root/tests/pb_heater_tick_host_test.c" \
  "$root/tests/pb_heater_tick_random.c" \
  "$dc_pid/dc_pid.c" \
  -lm -o "$out"

"$out" "$@"
