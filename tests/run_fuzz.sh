#!/bin/sh
# Build the libFuzzer harnesses in tests/fuzz and run each one.
# Usage: tests/run_fuzz.sh [SECONDS]   (default 30 s for each harness)
# The compiler must be clang with libFuzzer. Set FUZZ_CC to select one.
# New inputs go to a work directory. The seed corpus in the repo stays the same.
# tests/vendor/cjson is cJSON v1.7.19, the copy in ESP-IDF v5.3.6 components/json.
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
for c in pb_heater pb_ntc pb_leds pb_policy pb_buttons pb_fan pb_httpd db_portal pb_ha db_klipper_mqtt; do
  inc="$inc -I$root/components/$c/include"
done

# shellcheck disable=SC2086
"$cc" $flags $inc -I"$root/components/pb_httpd" \
  "$root/tests/fuzz/fuzz_httpd.c" \
  "$root/tests/fuzz/fuzz_host.c" \
  "$root/components/pb_policy/pb_policy.c" \
  "$root/tests/vendor/cjson/cJSON.c" \
  -lm -o "$work/fuzz_httpd"

# shellcheck disable=SC2086
"$cc" $flags $inc \
  "$root/tests/fuzz/fuzz_portal.c" \
  "$root/tests/fuzz/fuzz_host.c" \
  "$root/components/db_portal/db_portal.c" \
  "$root/components/db_portal/db_portal_config.c" \
  "$root/tests/vendor/cjson/cJSON.c" \
  -lm -o "$work/fuzz_portal"

# Input size limit: API_BODY_MAX of pb_httpd.c is 1536 and dc_portal reads at
# most 4096 bytes. A few requests of that size fit in 16384 bytes.
run() {
  name="$1"; shift
  mkdir -p "$work/corpus-$name" "$work/artifacts-$name"
  echo "== fuzz_$name for ${seconds} s"
  "$work/fuzz_$name" \
    -max_total_time="$seconds" -timeout=10 -max_len=16384 -print_final_stats=1 \
    -dict="$root/tests/fuzz/http.dict" \
    -artifact_prefix="$work/artifacts-$name/" "$@" \
    "$work/corpus-$name" "$root/tests/fuzz/corpus/$name"
}

run httpd
run portal
