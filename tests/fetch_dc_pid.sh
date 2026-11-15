#!/bin/sh
# Download dc_pid (dragon-core) at the version that main/idf_component.yml pins,
# into a cache directory, and print that directory. The host tests that need
# dc_pid use it. The directory holds include/dc_pid.h and dc_pid.c.
#
# Cache: $DC_PID_CACHE_DIR, else $XDG_CACHE_HOME/dragonbreath, else
# ~/.cache/dragonbreath. A version is downloaded only once.
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
version="$(awk '/^  dc_pid:/ { f = 1; next } f && /^  [^ ]/ { exit }
               f && $1 == "version:" { print $2; exit }' "$root/main/idf_component.yml")"
case "$version" in
  v[0-9]*.[0-9]*.[0-9]*) ;;
  *) echo "fetch_dc_pid.sh: no dc_pid version in main/idf_component.yml" >&2; exit 1 ;;
esac

cache="${DC_PID_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/dragonbreath}"
dir="$cache/dc_pid-$version"
if [ ! -f "$dir/dc_pid.c" ] || [ ! -f "$dir/include/dc_pid.h" ]; then
  mkdir -p "$cache"
  tmp="$(mktemp -d "$cache/.dc_pid-$version.XXXXXX")"
  base="https://raw.githubusercontent.com/justinh-rahb/dragon-core/$version/components/dc_pid"
  mkdir -p "$tmp/include"
  curl -fsSL "$base/include/dc_pid.h" -o "$tmp/include/dc_pid.h"
  curl -fsSL "$base/dc_pid.c" -o "$tmp/dc_pid.c"
  # Another run can fill the cache at the same time. Keep the first one.
  mv "$tmp" "$dir" 2>/dev/null || rm -rf "$tmp"
  [ -f "$dir/dc_pid.c" ] || { echo "fetch_dc_pid.sh: cache $dir is incomplete" >&2; exit 1; }
fi
printf '%s\n' "$dir"
