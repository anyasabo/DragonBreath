#!/bin/sh
# Test tools/check_core_pins.sh. The release manifest takes the dragon-core
# source and ref from one component. Thus all dragon-core components must have
# the same git URL and the same version.
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
check="$root/tools/check_core_pins.sh"
dir="$(mktemp -d "${TMPDIR:-/tmp}/core-pins.XXXXXX")"
trap 'rm -rf "$dir"' EXIT

cat > "$dir/same.yml" <<'EOF'
dependencies:
  dc_evlog:
    git: https://github.com/example/dragon-core.git
    path: components/dc_evlog
    version: 1111111111111111111111111111111111111111
  dc_portal:
    git: https://github.com/example/dragon-core.git
    path: components/dc_portal
    version: 1111111111111111111111111111111111111111
  idf:
    version: ">=5.3"
EOF

cat > "$dir/other_ref.yml" <<'EOF'
dependencies:
  dc_evlog:
    git: https://github.com/example/dragon-core.git
    path: components/dc_evlog
    version: 1111111111111111111111111111111111111111
  dc_portal:
    git: https://github.com/example/dragon-core.git
    path: components/dc_portal
    version: 2222222222222222222222222222222222222222
EOF
sed '0,/example\/dragon-core/s#example/dragon-core#fork/dragon-core#' \
    "$dir/same.yml" > "$dir/other_src.yml"
printf 'dependencies:\n  idf:\n    version: ">=5.3"\n' > "$dir/none.yml"

fail=0
expect() {
    want="$1"
    file="$2"
    if sh "$check" "$dir/$file" > "$dir/out" 2>&1; then got=0; else got=1; fi
    if [ "$got" -ne "$want" ]; then
        echo "FAIL $file: exit $got, expected $want"
        cat "$dir/out"
        fail=1
    fi
}

expect 0 same.yml
expect 1 other_ref.yml
expect 1 other_src.yml
expect 1 none.yml
# The real manifest of this tree must pass.
sh "$check" "$root/main/idf_component.yml" > /dev/null || { echo "FAIL main/idf_component.yml"; fail=1; }

[ "$fail" -eq 0 ] && echo "core pins: all cases pass"
exit "$fail"
