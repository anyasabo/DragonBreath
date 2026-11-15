#!/bin/sh
# Build the libFuzzer harnesses in tests/fuzz and run each one.
# Usage: tests/run_fuzz.sh [SECONDS]   (default 30 s for each harness)
# The compiler must be clang with libFuzzer. Set FUZZ_CC to select one.
# New inputs go to a work directory. The seed corpus in the repo stays the same.
# tests/vendor/cjson is cJSON v1.7.19, the copy in ESP-IDF v5.3.6 components/json.
#
# Merge order: the harnesses find real bugs in upstream main. The fixes are on
# fix/httpd-body-read (stalled and slow clients, the purge of an unread body)
# and fix/httpd-settings-validate (settings-bad, zones-target-300). Merge
# fix/httpd-body-read, then fix/httpd-settings-validate, then this branch.
# Without them, the seeds fail, for example settings-bad.req and the drip-*.req.
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
seconds="${1:-30}"
cc="${FUZZ_CC:-$(command -v clang-23 || command -v clang || true)}"
if [ -z "$cc" ]; then
  echo "error: no clang found. Install clang and libclang-rt-dev, or set FUZZ_CC." >&2
  exit 2
fi
work="${TMPDIR:-/tmp}/dragonbreath-fuzz"
# The C dialect and the warning flags are the ones of ESP-IDF: gnu17 (db_portal.c
# uses asm labels), and -Wextra without unused-parameter and sign-compare.
flags="-std=gnu17 -g -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all"

mkdir -p "$work"

# The stubs come first: they replace the ESP-IDF and dragon-core headers.
inc="-I$root/tests/fuzz/stubs -I$root/tests/fuzz -I$root/tests/vendor/cjson"
for c in pb_heater pb_ntc pb_leds pb_policy pb_buttons pb_fan pb_httpd db_portal pb_ha db_klipper_mqtt \
         pb_metrics pb_netlog; do
  inc="$inc -I$root/components/$c/include"
done

# shellcheck disable=SC2086
"$cc" $flags $inc -I"$root/components/pb_httpd" \
  "$root/tests/fuzz/fuzz_httpd.c" \
  "$root/tests/fuzz/fuzz_host.c" \
  "$root/components/pb_httpd/pb_auth.c" \
  "$root/components/pb_httpd/pb_httpd_replay.c" \
  "$root/components/pb_metrics/pb_metrics_fmt.c" \
  "$root/components/pb_metrics/pb_panic_record.c" \
  "$root/components/pb_policy/pb_policy.c" \
  "$root/tests/vendor/cjson/cJSON.c" \
  -lm -o "$work/fuzz_httpd"

# The same harness with the fork auth options on: CONFIG_DB_REQUIRE_CONTROL_TOKEN
# and CONFIG_DB_AUTH_LOCKOUT (the Kconfig defaults of the lockout).
strict="-DCONFIG_DB_REQUIRE_CONTROL_TOKEN=1 -DCONFIG_DB_AUTH_LOCKOUT=1"
strict="$strict -DCONFIG_DB_AUTH_LOCKOUT_MAX_FAILS=10 -DCONFIG_DB_AUTH_LOCKOUT_WINDOW_S=60"
strict="$strict -DCONFIG_DB_AUTH_LOCKOUT_DURATION_S=60"
# shellcheck disable=SC2086
"$cc" $flags $strict $inc -I"$root/components/pb_httpd" \
  "$root/tests/fuzz/fuzz_httpd.c" \
  "$root/tests/fuzz/fuzz_host.c" \
  "$root/components/pb_httpd/pb_auth.c" \
  "$root/components/pb_httpd/pb_httpd_replay.c" \
  "$root/components/pb_metrics/pb_metrics_fmt.c" \
  "$root/components/pb_metrics/pb_panic_record.c" \
  "$root/components/pb_policy/pb_policy.c" \
  "$root/tests/vendor/cjson/cJSON.c" \
  -lm -o "$work/fuzz_httpd_strict"

# shellcheck disable=SC2086
"$cc" $flags $inc \
  "$root/tests/fuzz/fuzz_portal.c" \
  "$root/tests/fuzz/fuzz_host.c" \
  "$root/components/db_portal/db_portal.c" \
  "$root/components/db_portal/db_portal_config.c" \
  "$root/components/pb_netlog/pb_netlog_config.c" \
  "$root/tests/vendor/cjson/cJSON.c" \
  -lm -o "$work/fuzz_portal"

# Input size limit: API_BODY_MAX of pb_httpd.c is 1536 and dc_portal reads at
# most 4096 bytes. A few requests of that size fit in 16384 bytes.
# Usage: run NAME CORPUS. The seed corpus is tests/fuzz/corpus/CORPUS.
run() {
  name="$1"; seeds="$2"; shift 2
  mkdir -p "$work/corpus-$name" "$work/artifacts-$name"
  echo "== fuzz_$name for ${seconds} s"
  "$work/fuzz_$name" \
    -max_total_time="$seconds" -timeout=10 -max_len=16384 -print_final_stats=1 \
    -dict="$root/tests/fuzz/http.dict" \
    -artifact_prefix="$work/artifacts-$name/" "$@" \
    "$work/corpus-$name" "$root/tests/fuzz/corpus/$seeds"
}

run httpd httpd
run httpd_strict httpd
run portal portal
