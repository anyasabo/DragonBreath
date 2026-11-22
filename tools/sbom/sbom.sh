#!/bin/sh
# SBOM, license check and vulnerability scans of the firmware image. CI and local runs use this script.
#
#   tools/sbom/sbom.sh create BUILD_DIR OUT_DIR   SBOMs, license check, notices, esp-idf-sbom check
#   tools/sbom/sbom.sh scan OUT_DIR               Grype on the CycloneDX SBOM of OUT_DIR, and a check for
#                                                 new releases of watched components (cJSON)
#   tools/sbom/sbom.sh nvdpp OUT_DIR              VulnCheck NVD++: CVEs that NVD has not analysed yet.
#                                                 Needs VULNCHECK_TOKEN (free community account).
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
    # cJSON has no fixed release for its 2026 CVEs (tools/sbom/vex.json). When a newer release
    # exists, bump espressif/cjson and review the cJSON statements. Renovate cannot see the ESP
    # Component Registry, so this check is the reminder.
    python3 -I - "$out/dragonbreath.cdx.json" >> "$out/summary.md" <<'EOF' || true
import json, sys, urllib.request
have = next((c["version"] for c in json.load(open(sys.argv[1]))["components"] if c.get("name") == "cJSON"), None)
req = urllib.request.Request("https://api.github.com/repos/DaveGamble/cJSON/releases/latest",
                             headers={"User-Agent": "dragonbreath-sbom"})
latest = json.load(urllib.request.urlopen(req, timeout=30))["tag_name"].lstrip("v")
print("## Watched components")
print()
if have and latest != have:
    print(f"- **cJSON {latest} is released; the image has {have}.** Bump espressif/cjson and review the cJSON statements in tools/sbom/vex.json.")
else:
    print(f"- cJSON: the image has {have}, the latest release is {latest}.")
print()
EOF
    ;;
nvdpp)
    [ $# -eq 2 ] || { echo "usage: $0 nvdpp OUT_DIR" >&2; exit 2; }
    out=$2
    [ -n "${VULNCHECK_TOKEN:-}" ] || { echo "sbom.sh: VULNCHECK_TOKEN is not set" >&2; exit 2; }
    cache=${XDG_CACHE_HOME:-$HOME/.cache}/vulncheck
    export_zip="$cache/nist-nvd2-$(date -u +%F).zip"
    mkdir -p "$cache"
    if [ ! -f "$export_zip" ]; then
        # The token goes on stdin, not on the command line.
        url=$(printf 'header = "Authorization: Bearer %s"\n' "$VULNCHECK_TOKEN" |
            curl -fsS -K - -A dragonbreath-sbom/1 https://api.vulncheck.com/v3/backup/nist-nvd2 |
            python3 -I -c 'import json,sys; print(json.load(sys.stdin)["data"][0]["url"])')
        curl -fsSL -o "$export_zip.tmp" "$url"
        mv "$export_zip.tmp" "$export_zip"
        find "$cache" -name 'nist-nvd2-*.zip' ! -name "$(basename "$export_zip")" -delete
    fi
    python3 -I "$here/match_nvdpp.py" --json "$out/dragonbreath.cdx.json" "$export_zip" \
        --vex "$out/esp-idf.vex.json" --vex "$here/vex.json" > "$out/nvdpp.json"
    python3 -I - "$out/nvdpp.json" >> "$out/summary.md" <<'EOF'
import json, sys
rows = json.load(open(sys.argv[1]))
print("## NVD++ (CVEs that NVD has not analysed yet)")
print()
print(f"- Open matches: {len(rows)}. Data: NVD++ by VulnCheck (https://vulncheck.com/nvd2).")
if rows:
    print()
    print("| component | CVE | NVD status | source |")
    print("|---|---|---|---|")
    for r in rows:
        print(f"| {r['component']} | {r['cve']} | {r['status'] or ''} | {'+'.join(r['sources'])} |")
print()
EOF
    ;;
*)
    echo "usage: $0 create BUILD_DIR OUT_DIR | scan OUT_DIR | nvdpp OUT_DIR" >&2
    exit 2
    ;;
esac
