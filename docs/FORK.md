# About this fork

This repository is a fork of [plastikman/DragonBreath](https://github.com/plastikman/DragonBreath). It adds safety fixes, security fixes, diagnostics, tests and supply-chain checks. Each change is also proposed upstream. When upstream merges a change, the fork drops its copy.

**Status:** the fork runs on one Panda Breath V1.1 (slim) with PCB V1.0.1, on a Prusa CORE One L with PrusaLink. It has not completed a heat test on mains power yet. If you want a firmware that other users run, use the upstream release until then.

## Branches

| Branch | Content |
| --- | --- |
| `fork` (default) | The firmware that this fork publishes. It is upstream `main`, all topic branches and the fork-only commits. |
| `main` | A copy of upstream `main`. It has no fork changes. |
| Topic branches (`fix/...`, `feat/...`, `test/...`, `ci/...`, `build/...`, `deps/...`) | One change each, from upstream `main`. Each one is a candidate for an upstream PR. Exceptions: `feat/first-heat-check` contains `fix/heater-fault-persist` and `test/heater-control-tick`. `build/devboard-profile` contains `build/idf-v6`. `test/heater-metrics-counters` is based on `fork`. |

The shared components come from the fork [anyasabo/dragon-core](https://github.com/anyasabo/dragon-core), branch `fork`, pinned to one commit in [`main/idf_component.yml`](../main/idf_component.yml). Upstream DragonBreath uses [justinh-rahb/dragon-core](https://github.com/justinh-rahb/dragon-core).

To see all differences from upstream:

```bash
git remote add upstream https://github.com/plastikman/DragonBreath.git
git fetch upstream
git log --oneline --no-merges upstream/main..fork
git diff upstream/main...fork --stat
```

## Changes that you see as a user

These changes affect the use of a release build of this fork. Read them before you install it.

1. **A control token is necessary.** The release build sets `CONFIG_DB_REQUIRE_CONTROL_TOKEN`. If no token is set, the device refuses LAN control and OTA. The stock firmware has no token, so install the upstream release first and set the token there (see [Install](#install)). If the fork runs with no token, see [Recovery](#recovery-from-a-lockout).
2. **The setup AP starts only after a failed Wi-Fi join at boot.** The release build sets the AP mode to "fallback". Upstream uses "always". A mode that you set on the setup page has priority.
3. **First-heat check.** Until a pass is stored, the first heat after each boot checks that the PTC sensor rises first. If the chamber sensor rises first, or if the PTC sensor does not rise, the heat stops and a fault latches. The firmware never swaps the sensors by itself. The thresholds are provisional, because no capture from real hardware has set them yet. See [SAFETY.md](SAFETY.md#first-heat-check-thermistor-roles-and-element).
4. **A hazard trip during a panic-off or an inhibit survives a power cycle.** Upstream keeps such a trip in RAM only.
5. **Stricter input checks.** For a request to `/settings` with a bad field, the firmware applies no field. A zone target above 255 is clamped, not wrapped. `/api/v2/presets` and `/api/v2/bambu/scan` need the auth header.
6. **A slow client cannot hold the web server.** A body read stops after 3 receive timeouts in a row or after approximately 10 s. The worst case is 13 s. Socket timeouts are 3 s.
7. **ESP-IDF v6.1.1.** Upstream builds with ESP-IDF v5.3. The fork image uses ESP-IDF v6.1.1, with a new C library (picolibc), GCC 15, Mbed TLS 4 and new Wi-Fi binaries. The image also has a stack protector and light heap poisoning. If a function with a local array returns after an overrun of that array, the stack protector stops the firmware with a panic. If `free()` finds a damaged block border, heap poisoning stops it.
8. **New endpoints and settings:** `GET /metrics` (Prometheus), syslog over UDP and SNTP time (Setup, "Syslog and time"). All are off or read-only by default.

The [API documentation](api-v2.md) and [SAFETY.md](SAFETY.md) on this branch include these changes.

## Checks

- **CI** runs on each push to `fork` and each Monday. It runs the host tests with gcc-16 and with clang-23 + ASan/UBSan, and coverage. It also runs cppcheck 2.21, the fuzzers, the release build, a build with ESP-IDF v5.3.6, the static checks on the ELF and the SBOM checks.
- **Compiler checks:** the product and dragon-core components build with `-Werror` and more warnings as errors. `-DDB_EXTRA_WARNINGS=ON -DDB_ANALYZER=ON` makes an analysis build with more warnings and the GCC static analyzer.
- **SBOM, CVEs and licenses:** [`tools/sbom/sbom.sh`](../tools/sbom/sbom.sh) makes SPDX and CycloneDX SBOMs of the linked components. It checks them with `esp-idf-sbom check` and Grype. If a token is set, it also checks them with VulnCheck NVD++. [`tools/sbom/vex.json`](../tools/sbom/vex.json) (OpenVEX) gives the reason for each reported CVE that does not apply to the image. A CVE only reports and does not fail CI. The license check fails on a copyleft or an unknown license in the image.
- **Reproducible images:** the same commit and the same ESP-IDF version give the same image, also in CI. Thus you can check a release against the source.

## Changes and their upstream status

"Draft N" means that the issue or PR text is ready and not posted yet. Lower numbers go first.

### Safety and security

| Change | Branch | Upstream |
| --- | --- | --- |
| A hazard trip on top of a RAM-only latch goes to NVS. A failed clear leaves flash latched. | `fix/heater-fault-persist-only` (PR), `fix/heater-fault-persist` (with the host test) | Draft 1 (wave 1) |
| The setup portal closes its open routes after a late Wi-Fi join. The Wi-Fi scan needs auth in station mode. | dragon-core `fix/portal-setup-auth` | Draft 2 (wave 1) |
| A stalled client cannot hold the web server. Bodies are read in full. | `fix/httpd-body-read`, `fix/httpd-timeouts` | Draft 7 (wave 2) |
| The same fix for the portal and the OTA upload | dragon-core `fix/portal-body-read` | Draft 8 (wave 2) |
| `/settings` checks all fields before it applies one. Zone targets are clamped. | `fix/httpd-settings-validate` | Draft 6 (wave 2) |
| Auth on presets and the Bambu scan. Constant-time token compare. | `fix/portal-auth-presets-scan` (PR) | Draft 9 (wave 2) |
| Optional "require token" and auth lockout | `feat/auth-hardening` | Later |
| Fail-closed first-heat check | `feat/first-heat-check` | Waits for a capture from real hardware |

### Diagnostics

| Change | Branch | Upstream |
| --- | --- | --- |
| `GET /metrics` in Prometheus format | `feat/metrics-endpoint` | Waits for data from real hardware |
| Tick histogram, panic breadcrumb, health counters | `feat/metrics-diagnostics` | Later |
| Syslog (RFC 5424 over UDP) and SNTP | `feat/syslog-time`, dragon-core `feat/dc-syslog`, `feat/dc-time`, `fix/dc-time-dhcp-late` | Comment drafted for dragon-core #69 |
| The status line moves out of the control task. The replay cache moves to the heap. | `fix/control-task-status-line`, `fix/httpd-replay-heap` | Not proposed yet |

### Tests, CI and tools

| Change | Branch | Upstream |
| --- | --- | --- |
| One host-test runner, sanitizer CI matrix, coverage | `ci/host-test-hardening` | Draft 17 (wave 1) |
| Reproducible release images | `build/reproducible` | Draft 18 (wave 2) |
| cppcheck 2.21 in a container | `ci/cppcheck-2.21` | Draft 19 (wave 2) |
| `-Werror` on `db_*` and `main` | `build/werror-db-main` (PR) | Draft 4 (wave 2) |
| Release checksums list only the published files. The manifest lists all components. | `fix/release-assets` | Draft 3 (wave 2) |
| ESP-IDF v6.1 support (v5.3 support ends in January 2027) | `build/idf-v6`, dragon-core `deps/led-strip-3`, `build/idf-v6-core` | Draft 11: the issue in wave 1, the PRs later |
| Host test of the real `pb_heater.c` with randomized invariants | `test/heater-control-tick` | Offered with the fault-latch fix |
| libFuzzer for the HTTP handlers and the dragon-core parsers | `test/fuzz-http-handlers`, dragon-core `test/fuzz-parsers` | Not proposed yet |
| Static checks on the ELF (calls from the control task, worst-case stack) | `ci/elf-static-checks` | Not proposed yet |
| `.idf-version` is the one place for the ESP-IDF version | `build/idf-version-file` | Not proposed yet |
| SBOM (SPDX) in CI and releases | `deps/sbom-dependabot` | Not proposed yet |
| `flash.py` and `hil.py` declare their dependencies (PEP 723). The flash backup is read in chunks. | `build/pep723-script-deps`, `fix/flash-read-chunked` | Not proposed yet |
| Smoke test, capture and replay tools | `feat/network-smoke-test`, `feat/capture-tools` | Not proposed yet |

### Fork-only

These commits stay in this fork. They are not for upstream.

- The release settings in [`sdkconfig.release`](../sdkconfig.release): require a token, AP mode "fallback", stack protector `strong`, light heap poisoning.
- The pins to the dragon-core fork, and `dependencies.lock` in git.
- ESP-IDF v6.1.1 in [`.idf-version`](../.idf-version). CI also checks that the code builds with v5.3.6.
- The compiler checks in [`CMakeLists.txt`](../CMakeLists.txt) beyond `build/werror-db-main`: `-Werror` for the dragon-core components, `DB_STRICT_WARNING_FLAGS`, the analysis build.
- The SBOM, CVE and license tools in [`tools/sbom/`](../tools/sbom/), and the release assets that they make.
- CI: the trigger for `fork`, clang-23 from apt.llvm.org, the container image pinned by digest, a weekly run, [Renovate](../renovate.json). The Pages workflow is turned off on the fork.
- Integration fixes for two topic branches together. Example: `/settings` refuses `swap_thermistors` together with `first_heat_apply_swap` (from `feat/first-heat-check` and `fix/httpd-settings-validate`). Others adapt the fuzzers, the ELF check allowlist and the release manifest to all branches together.

To list the fork-only commits: `git log --oneline --no-merges upstream/main..fork`, then remove the commits of each topic branch.

## Install

The fork has no published release yet. Build the image from the branch `fork` with the release settings, in an empty build directory. Give the build its own `sdkconfig`: ESP-IDF applies the defaults files only to a new `sdkconfig`, and an old `./sdkconfig` from a dev build has no token requirement.

```bash
./tools/idf-build.sh . esp32c3 build-release -DSDKCONFIG="$PWD/build-release/sdkconfig" -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release" build
# The image is build-release/dragonbreath.bin.
```

1. Make a backup of the stock firmware (see the [README](../README.md#install--update)).
2. Install the upstream release over the stock firmware, as the README tells.
3. On the upstream firmware, set a control token (Settings, or `POST /api/v2/token`) and a new setup AP password.
4. Upload the fork image through `/fw`, with the token.
5. Run the smoke test: `python3 tools/smoke.py HOST --token TOKEN` ([SMOKE_TEST.md](SMOKE_TEST.md)).

### Recovery from a lockout

If the fork runs with no token, it refuses LAN control and OTA.

1. Do a front-panel reset (Power+Auto). This erases the settings and the Wi-Fi credentials, and the setup AP starts.
2. Connect to the setup AP.
3. Set the token first, in the "Security" part of the setup page. If you save Wi-Fi first, the device restarts and joins your Wi-Fi with no token, and the setup AP stays off.

To go back to upstream, install an upstream release image through `/fw`. The fork stores some settings in NVS that upstream ignores. A front-panel reset erases them.
