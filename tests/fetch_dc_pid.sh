#!/bin/sh
# Download dc_pid (dragon-core) at the version that main/idf_component.yml pins,
# into a cache directory, and print that directory. The host tests that need
# dc_pid use it. The directory holds include/dc_pid.h and dc_pid.c.
#
# The script reads the "git:" URL and the "version:" of the dc_pid entry.
#   - version is a 40-hex commit hash: git fetches exactly that commit, so git
#     gives the integrity. No table entry is necessary.
#   - version is a tag: tags can move, so the script downloads the two raw files
#     and checks their SHA-256 against the table below. A new tag needs a new line.
# In both cases the cache stores the SHA-256 of the files and checks it on each
# cache hit.
#
# Cache: $DC_PID_CACHE_DIR, else $XDG_CACHE_HOME/dragonbreath, else
# ~/.cache/dragonbreath. Needs only POSIX sh, git, curl and sha256sum.
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
manifest_value() {   # manifest_value KEY: the KEY of the dc_pid entry, without quotes
  awk -v key="$1:" '/^  dc_pid:/ { f = 1; next } f && /^  [^ ]/ { exit }
      f && $1 == key { v = $2; gsub(/["'\'']/, "", v); print v; exit }' \
      "$root/main/idf_component.yml"
}
url="$(manifest_value git)"
version="$(manifest_value version)"
if [ -z "$url" ] || [ -z "$version" ]; then
  echo "fetch_dc_pid.sh: no dc_pid git URL or version in main/idf_component.yml" >&2
  exit 1
fi

# owner/repo from https://github.com/OWNER/REPO(.git)
slug="$(printf '%s\n' "$url" | sed -n 's#^https://github\.com/\([^/]*\)/\([^/]*\)$#\1/\2#p' |
        sed 's#\.git$##')"
if [ -z "$slug" ]; then
  echo "fetch_dc_pid.sh: dc_pid git URL is not https://github.com/OWNER/REPO: $url" >&2
  exit 1
fi

case "$version" in
  *[!0-9a-f]*) mode=tag ;;
  *) if [ "${#version}" -eq 40 ]; then mode=commit; else mode=tag; fi ;;
esac

# Pinned tags: owner/repo  tag  sha256(dc_pid.c)  sha256(include/dc_pid.h)
known="
justinh-rahb/dragon-core v0.36.0 46eba6416a2669fa09ebf3f8e1a5181a7f46677274f2dbbb06ae6e1c522e7cdc 545a784334fd0cc159fc2de1a8a3adddaddd74a3cc7a140852e3a4a8b98a1551
"

sha() { sha256sum "$1" | awk '{ print $1 }'; }
# valid DIR SUM_C SUM_H: both files exist and match the hashes
valid() {
  [ -f "$1/dc_pid.c" ] && [ -f "$1/include/dc_pid.h" ] &&
  [ "$(sha "$1/dc_pid.c")" = "$2" ] && [ "$(sha "$1/include/dc_pid.h")" = "$3" ]
}
# cache_valid DIR: the files match the hashes that the cache stored at download
cache_valid() {
  [ -f "$1/SHA256SUMS" ] || return 1
  valid "$1" "$(awk '$2 == "dc_pid.c" { print $1 }' "$1/SHA256SUMS")" \
             "$(awk '$2 == "include/dc_pid.h" { print $1 }' "$1/SHA256SUMS")"
}

sum_c=""
sum_h=""
if [ "$mode" = tag ]; then
  line="$(printf '%s\n' "$known" | awk -v s="$slug" -v v="$version" '$1 == s && $2 == v { print; exit }')"
  if [ -z "$line" ]; then
    echo "fetch_dc_pid.sh: dc_pid tag $slug $version has no SHA-256 entry in" \
         "tests/fetch_dc_pid.sh (pin a commit hash, or add the entry)" >&2
    exit 1
  fi
  sum_c="$(printf '%s\n' "$line" | awk '{ print $3 }')"
  sum_h="$(printf '%s\n' "$line" | awk '{ print $4 }')"
fi
# usable DIR: a cache entry that we can use without a download
usable() {
  cache_valid "$1" || return 1
  [ "$mode" = commit ] || valid "$1" "$sum_c" "$sum_h"
}

cache="${DC_PID_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/dragonbreath}"
dir="$cache/dc_pid-$(printf '%s' "$slug" | tr / -)-$version"
if ! usable "$dir"; then
  mkdir -p "$cache"
  tmp="$(mktemp -d "$cache/.dc_pid-$version.XXXXXX")"
  trap 'rm -rf "$tmp"' EXIT
  mkdir -p "$tmp/include"
  if [ "$mode" = commit ]; then
    git init -q "$tmp/git"
    # Stop a stalled transfer (below 1 KB/s for 30 s).
    if ! git -C "$tmp/git" -c http.lowSpeedLimit=1000 -c http.lowSpeedTime=30 \
         fetch -q --depth 1 "$url" "$version"; then
      echo "fetch_dc_pid.sh: git fetch of $url $version failed" >&2
      exit 1
    fi
    if [ "$(git -C "$tmp/git" rev-parse FETCH_HEAD)" != "$version" ]; then
      echo "fetch_dc_pid.sh: git fetched a different commit than $version" >&2
      exit 1
    fi
    git -C "$tmp/git" show "$version:components/dc_pid/dc_pid.c" > "$tmp/dc_pid.c"
    git -C "$tmp/git" show "$version:components/dc_pid/include/dc_pid.h" > "$tmp/include/dc_pid.h"
    rm -rf "$tmp/git"
  else
    base="https://raw.githubusercontent.com/$slug/$version/components/dc_pid"
    for f in include/dc_pid.h dc_pid.c; do
      curl -fsSL --connect-timeout 10 --max-time 60 --retry 2 "$base/$f" -o "$tmp/$f"
    done
    if ! valid "$tmp" "$sum_c" "$sum_h"; then
      echo "fetch_dc_pid.sh: SHA-256 mismatch for dc_pid $slug $version" >&2
      exit 1
    fi
  fi
  printf '%s  %s\n' "$(sha "$tmp/dc_pid.c")" dc_pid.c \
                    "$(sha "$tmp/include/dc_pid.h")" include/dc_pid.h > "$tmp/SHA256SUMS"
  chmod 755 "$tmp"
  # Replace a bad cache entry. If another run filled it meanwhile, keep that one.
  if [ -e "$dir" ] && ! usable "$dir"; then rm -rf "$dir"; fi
  mv -T "$tmp" "$dir" 2>/dev/null || true
  if ! usable "$dir"; then
    echo "fetch_dc_pid.sh: cache $dir is not valid" >&2
    exit 1
  fi
fi
printf '%s\n' "$dir"
