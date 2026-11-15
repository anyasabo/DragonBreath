#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
tmp="${TMPDIR:-/tmp}/dragonbreath-heater-tick-dc-pid"
out="${TMPDIR:-/tmp}/dragonbreath-pb-heater-tick-test"

# Compile the REAL pb_heater.c with the stubs in tests/stubs_heater (first on the
# include path) and the dc_pid version that main/idf_component.yml pins.
dc_pid_version=v0.36.0
base="https://raw.githubusercontent.com/justinh-rahb/dragon-core/$dc_pid_version/components/dc_pid"
mkdir -p "$tmp/include"
curl -fsSL "$base/include/dc_pid.h" -o "$tmp/include/dc_pid.h"
curl -fsSL "$base/dc_pid.c" -o "$tmp/dc_pid.c"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${HOST_TEST_CFLAGS:-} \
  -I"$root/tests/stubs_heater" \
  -I"$root/components/pb_heater/include" \
  -I"$root/components/pb_ntc/include" \
  -I"$root/components/pb_board/include" \
  -I"$root/tests/stubs" \
  -I"$root/tests" \
  -I"$tmp/include" \
  "$root/components/pb_heater/pb_heater.c" \
  "$root/tests/heater_host_env.c" \
  "$root/tests/pb_heater_tick_host_test.c" \
  "$root/tests/pb_heater_tick_random.c" \
  "$tmp/dc_pid.c" \
  -lm -o "$out"

"$out" "$@"
