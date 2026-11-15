#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
# Compile the REAL pb_heater.c as the safe dev-board profile builds it
# (CONFIG_PB_DEVBOARD_SAFE): the first-heat check must report only.
dc_pid="$(sh "$root/tests/fetch_dc_pid.sh")"
out="$(mktemp "${TMPDIR:-/tmp}/dragonbreath-pb-heater-fh-devboard-test.XXXXXX")"
trap 'rm -f "$out"' EXIT

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${HOST_TEST_CFLAGS:-} \
  -DCONFIG_PB_DEVBOARD_SAFE=1 \
  -I"$root/tests/stubs_heater" \
  -I"$root/components/pb_heater/include" \
  -I"$root/components/pb_ntc/include" \
  -I"$root/components/pb_board/include" \
  -I"$root/tests/stubs" \
  -I"$root/tests" \
  -I"$dc_pid/include" \
  "$root/components/pb_heater/pb_heater.c" \
  "$root/tests/heater_host_env.c" \
  "$root/tests/pb_heater_first_heat_devboard_test.c" \
  "$dc_pid/dc_pid.c" \
  -lm -o "$out"

"$out"
