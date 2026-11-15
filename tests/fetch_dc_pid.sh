#!/bin/sh
# Download dc_pid (dragon-core) at the version that main/idf_component.yml pins,
# into a cache directory, and print that directory. The host tests that need
# dc_pid use it. The directory holds include/dc_pid.h and dc_pid.c.
#
# Cache: $DC_PID_CACHE_DIR, else $XDG_CACHE_HOME/dragonbreath, else
# ~/.cache/dragonbreath. A version is downloaded only once. The script checks
# the SHA-256 of both files against the table below, after a download and on
# each cache hit. A new dc_pid version needs a new line in the table.
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
version="$(awk '/^  dc_pid:/ { f = 1; next } f && /^  [^ ]/ { exit }
               f && $1 == "version:" { v = $2; gsub(/["'\'']/, "", v); print v; exit }' \
           "$root/main/idf_component.yml")"

# version  sha256(dc_pid.c)  sha256(include/dc_pid.h)
known="
v0.36.0 46eba6416a2669fa09ebf3f8e1a5181a7f46677274f2dbbb06ae6e1c522e7cdc 545a784334fd0cc159fc2de1a8a3adddaddd74a3cc7a140852e3a4a8b98a1551
"
line="$(printf '%s\n' "$known" | awk -v v="$version" '$1 == v { print; exit }')"
if [ -z "$version" ] || [ -z "$line" ]; then
  echo "fetch_dc_pid.sh: dc_pid version '$version' from main/idf_component.yml has no" \
       "SHA-256 entry in tests/fetch_dc_pid.sh" >&2
  exit 1
fi
sum_c="$(printf '%s\n' "$line" | awk '{ print $2 }')"
sum_h="$(printf '%s\n' "$line" | awk '{ print $3 }')"

sha() { sha256sum "$1" | awk '{ print $1 }'; }
valid() {   # valid DIR: both files exist and match the pinned hashes
  [ -f "$1/dc_pid.c" ] && [ -f "$1/include/dc_pid.h" ] &&
  [ "$(sha "$1/dc_pid.c")" = "$sum_c" ] && [ "$(sha "$1/include/dc_pid.h")" = "$sum_h" ]
}

cache="${DC_PID_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/dragonbreath}"
dir="$cache/dc_pid-$version"
if ! valid "$dir"; then
  mkdir -p "$cache"
  tmp="$(mktemp -d "$cache/.dc_pid-$version.XXXXXX")"
  trap 'rm -rf "$tmp"' EXIT
  base="https://raw.githubusercontent.com/justinh-rahb/dragon-core/$version/components/dc_pid"
  mkdir -p "$tmp/include"
  for f in include/dc_pid.h dc_pid.c; do
    curl -fsSL --connect-timeout 10 --max-time 60 --retry 2 "$base/$f" -o "$tmp/$f"
  done
  if ! valid "$tmp"; then
    echo "fetch_dc_pid.sh: SHA-256 mismatch for dc_pid $version download" >&2
    exit 1
  fi
  # Replace a bad cache entry. If another run filled it meanwhile, keep that one.
  if [ -e "$dir" ] && ! valid "$dir"; then rm -rf "$dir"; fi
  chmod 755 "$tmp"
  mv -T "$tmp" "$dir" 2>/dev/null || true
  if ! valid "$dir"; then
    echo "fetch_dc_pid.sh: cache $dir is not valid" >&2
    exit 1
  fi
fi
printf '%s\n' "$dir"
