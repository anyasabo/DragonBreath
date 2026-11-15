#!/bin/sh
# Fail if a production build does not enforce the first-heat check: the build
# config must not set CONFIG_PB_FIRST_HEAT_REPORT_ONLY (the recovery build) or
# CONFIG_PB_DEVBOARD_SAFE. Also fail if a tracked sdkconfig file sets it.
#   sh tests/check_first_heat_enforced.sh [BUILD_DIR]   (default: build)
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
build_dir="${1:-build}"
config="$build_dir/config/sdkconfig.h"

if [ ! -f "$config" ]; then
    echo "missing build config: $config" >&2
    exit 1
fi
for opt in PB_FIRST_HEAT_REPORT_ONLY PB_DEVBOARD_SAFE; do
    if grep -q "^#define CONFIG_$opt 1$" "$config"; then
        echo "production build sets CONFIG_$opt: the first-heat check would report only" >&2
        exit 1
    fi
done
for f in "$root"/sdkconfig.defaults "$root"/sdkconfig.release; do
    if [ -f "$f" ] && grep -q '^CONFIG_PB_FIRST_HEAT_REPORT_ONLY=y' "$f"; then
        echo "$f sets CONFIG_PB_FIRST_HEAT_REPORT_ONLY=y" >&2
        exit 1
    fi
done
echo "first-heat enforcement check: PASS"
