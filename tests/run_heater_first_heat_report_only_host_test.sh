#!/bin/sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
# Compile the REAL pb_heater.c twice in report-only mode: as the safe dev-board
# profile builds it (CONFIG_PB_DEVBOARD_SAFE), and as a production build with
# CONFIG_PB_FIRST_HEAT_REPORT_ONLY=y. The first-heat check must report only.
dc_pid="$(sh "$root/tests/fetch_dc_pid.sh")"
out="$(mktemp "${TMPDIR:-/tmp}/dragonbreath-pb-heater-fh-report-only-test.XXXXXX")"
trap 'rm -f "$out"' EXIT

for defs in "-DCONFIG_PB_DEVBOARD_SAFE=1" "-DCONFIG_PB_FIRST_HEAT_REPORT_ONLY=1"; do
    # shellcheck disable=SC2086 # $defs is a list of flags
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${HOST_TEST_CFLAGS:-} $defs \
      -I"$root/tests/stubs_heater" \
      -I"$root/components/pb_heater/include" \
      -I"$root/components/pb_ntc/include" \
      -I"$root/components/pb_board/include" \
      -I"$root/tests/stubs" \
      -I"$root/tests" \
      -I"$dc_pid/include" \
      "$root/components/pb_heater/pb_heater.c" \
      "$root/tests/heater_host_env.c" \
      "$root/tests/pb_heater_first_heat_report_only_test.c" \
      "$dc_pid/dc_pid.c" \
      -lm -o "$out"
    "$out"
done
