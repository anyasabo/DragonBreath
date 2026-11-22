#!/bin/sh
# SBOM, license check and vulnerability scans of the firmware image. CI and local runs use this script.
#
#   tools/sbom/sbom.sh create BUILD_DIR OUT_DIR   SBOMs, license check, notices, esp-idf-sbom check
#   tools/sbom/sbom.sh scan OUT_DIR               Grype on the CycloneDX SBOM of OUT_DIR
#
# "create" must run where ESP-IDF built BUILD_DIR: project_description.json has absolute paths.
# In CI that is the ESP-IDF container. "scan" can run anywhere and needs grype on PATH
# (tools/ci/install-scanners.sh, or the devspace).
#
# Exit status: 1 if the license check fails or a tool fails. A vulnerability finding does not fail
# the script: it is in OUT_DIR/summary.md. Decide each finding in tools/sbom/vex.json (OpenVEX).
set -eu

ESP_IDF_SBOM_VERSION=1.5.0

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cmd=${1:-}

esp_idf_sbom() {
    if [ ! -x "$venv/bin/esp-idf-sbom" ]; then
        python3 -m venv "$venv"
        "$venv/bin/python" -m pip install --quiet "esp-idf-sbom==$ESP_IDF_SBOM_VERSION"
    fi
    COLUMNS=200 "$venv/bin/esp-idf-sbom" --no-color --no-progress --no-hint "$@"
}

case "$cmd" in
create)
    [ $# -eq 3 ] || { echo "usage: $0 create BUILD_DIR OUT_DIR" >&2; exit 2; }
    build=$2 out=$3
    desc="$build/project_description.json"
    [ -f "$desc" ] || { echo "sbom.sh: $desc not found. Build first." >&2; exit 2; }
    mkdir -p "$out"
    venv="$out/.venv"

    # Only the linked components (--rem-unused), and no configuration-only components such as
    # bt (--rem-config). Without --rem-config, the CycloneDX output lists NimBLE, which is not linked.
    esp_idf_sbom create --rem-unused --rem-config -o "$out/dragonbreath.spdx" "$desc"
    esp_idf_sbom create --rem-unused --rem-config --format cyclonedx-json \
        --vex-format openvex --vex-output "$out/esp-idf.vex.json" \
        -o "$out/dragonbreath.cdx.json" "$desc"

    # Licenses: the check fails the script. The notices go into each release.
    esp_idf_sbom license --format json --packages -o "$out/licenses.json" "$desc"
    esp_idf_sbom license --unify-copyrights -o "$out/licenses.txt" "$desc"
    # The toolchain has the picolibc license file. Find it from the compiler on PATH (idf.py
    # environment) or in the ESP-IDF tools directory (export.sh of a devspace install).
    picolibc=
    gcc=$(command -v riscv32-esp-elf-gcc || true)
    for f in ${gcc:+"$(dirname "$gcc")/../share/licenses/picolibc/COPYING.picolibc"} \
             "${IDF_TOOLS_PATH:-$HOME/.espressif}"/tools/riscv32-esp-elf/*/riscv32-esp-elf/share/licenses/picolibc/COPYING.picolibc; do
        if [ -f "$f" ]; then picolibc=$f; break; fi
    done
    if [ -n "$picolibc" ]; then
        { printf '\n\n==== picolibc (COPYING.picolibc of the toolchain) ====\n\n'; cat "$picolibc"; } >> "$out/licenses.txt"
    else
        echo "sbom.sh: warning: COPYING.picolibc not found; licenses.txt has no picolibc text" >&2
    fi
    python3 -I "$here/license_check.py" "$out/licenses.json" "$here/license-policy.json" | tee "$out/license-check.txt"

    # Vulnerabilities: report only. check exits 1 if a CVE is open.
    rc=0
    esp_idf_sbom check --vex "$here/vex.json" --format markdown -o "$out/esp-idf-sbom-check.md" \
        "$out/dragonbreath.spdx" || rc=$?
    esp_idf_sbom check --vex "$here/vex.json" --format json -o "$out/esp-idf-sbom-check.json" \
        "$out/dragonbreath.spdx" || true
    open=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["cves_summary"]["total_cves_count"])' \
        "$out/esp-idf-sbom-check.json")
    {
        echo "## SBOM: esp-idf-sbom $ESP_IDF_SBOM_VERSION"
        echo
        echo "- License check: $(tail -1 "$out/license-check.txt")"
        echo "- Open CVEs after tools/sbom/vex.json: $open (check exit code $rc)"
        echo
        if [ "$open" != 0 ]; then
            sed -n '/^|/p' "$out/esp-idf-sbom-check.md" | grep -E '^\| *(Package|-|.*YES|.*MAYBE)' || true
            echo
        fi
    } > "$out/summary.md"
    ;;
scan)
    [ $# -eq 2 ] || { echo "usage: $0 scan OUT_DIR" >&2; exit 2; }
    out=$2
    command -v grype >/dev/null || { echo "sbom.sh: grype not found (tools/ci/install-scanners.sh)" >&2; exit 2; }
    # Espressif's statements and ours. Grype applies "not_affected" statements as ignore rules.
    grype "sbom:$out/dragonbreath.cdx.json" --vex "$out/esp-idf.vex.json" --vex "$here/vex.json" \
        -o json --file "$out/grype.json" -q
    python3 -I - "$out/grype.json" >> "$out/summary.md" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
m = d["matches"]
print(f"## Grype {d['descriptor']['version']}")
print()
print(f"- Open matches: {len(m)}. Matches that a VEX statement decides: {len(d.get('ignoredMatches', []))}.")
if m:
    print()
    print("| severity | package | version | vulnerability |")
    print("|---|---|---|---|")
    seen = set()
    for x in m:
        a, v = x["artifact"], x["vulnerability"]
        k = (a["name"], a["version"], v["id"])
        if k not in seen:
            seen.add(k)
            print(f"| {v.get('severity', '?')} | {a['name']} | {a['version']} | {v['id']} |")
print()
EOF
    ;;
*)
    echo "usage: $0 create BUILD_DIR OUT_DIR | scan OUT_DIR" >&2
    exit 2
    ;;
esac
