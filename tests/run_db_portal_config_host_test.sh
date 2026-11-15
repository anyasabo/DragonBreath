#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="${TMPDIR:-/tmp}/dragonbreath-db-portal-config-test"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${HOST_TEST_CFLAGS:-} \
  -I"$root/tests/stubs" \
  -I"$root/components/db_portal/include" \
  -I"$root/components/db_klipper_mqtt/include" \
  -I"$root/components/pb_ha/include" \
  -I"$root/components/pb_netlog/include" \
  "$root/components/db_portal/db_portal_config.c" \
  "$root/components/pb_netlog/pb_netlog_config.c" \
  "$root/tests/db_portal_config_host_test.c" \
  -o "$out"

"$out"
