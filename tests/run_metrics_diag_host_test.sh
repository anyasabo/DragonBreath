#!/bin/sh
# Host test for the airflow signs and the panic breadcrumb record (pb_metrics).
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out="${TMPDIR:-/tmp}/dragonbreath-pb-metrics-diag-test"

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${HOST_TEST_CFLAGS:-} \
  -I"$root/components/pb_metrics/include" \
  "$root/components/pb_metrics/pb_metrics_airflow.c" \
  "$root/components/pb_metrics/pb_panic_record.c" \
  "$root/tests/pb_metrics_diag_host_test.c" \
  -o "$out"

"$out"
