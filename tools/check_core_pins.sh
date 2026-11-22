#!/bin/sh
# Check that all dragon-core components in an idf_component.yml have the same
# git URL and the same version.
# Usage: tools/check_core_pins.sh [main/idf_component.yml]
#
# tools/package_release.sh writes one source and one ref for all dragon-core
# components into the release manifest. If one component has another pin,
# the manifest is wrong. Upstream pinned two components to another fork once
# (DragonBreath 2806625).
set -eu

file="${1:-main/idf_component.yml}"
awk '
    /^  [A-Za-z0-9_.-]+:$/ { name = $1; sub(/:$/, "", name); src = "" }
    /^    git: .*dragon-core/ { src = $2 }
    /^    version:/ && src != "" { print name, src, $2; src = "" }
' "$file" | awk -v file="$file" '
    { n++; pin = $2 " " $3; if (!(pin in seen)) { seen[pin] = $1; pins++ } }
    END {
        if (n == 0) { print "check_core_pins: no dragon-core components in " file > "/dev/stderr"; exit 1 }
        if (pins > 1) {
            print "check_core_pins: dragon-core components in " file " have " pins " pins:" > "/dev/stderr"
            for (p in seen) print "  " p " (first: " seen[p] ")" > "/dev/stderr"
            exit 1
        }
    }
'
