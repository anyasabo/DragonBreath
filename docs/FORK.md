# About this fork

This repository is a fork of [plastikman/DragonBreath](https://github.com/plastikman/DragonBreath). It adds safety fixes, security fixes, diagnostics and tests. Each change is also proposed upstream. When upstream merges a change, the fork drops its copy.

**Status:** the fork runs on one Panda Breath V1.1 (slim) with PCB V1.0.1, on a Prusa CORE One L with PrusaLink. It has not completed a heat test on mains power yet. If you want a firmware that other users run, use the upstream release until then.

## Branches

| Branch | Content |
| --- | --- |
| `fork` (default) | The firmware that this fork publishes. It is upstream `main`, all topic branches and the fork-only commits. |
| `main` | A copy of upstream `main`. It has no fork changes. |
| Topic branches (`fix/...`, `feat/...`, `test/...`, `ci/...`, `build/...`, `deps/...`) | One change each, from upstream `main`. Each one is a candidate for an upstream PR. |

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

1. **A control token is necessary.** The release build sets `CONFIG_DB_REQUIRE_CONTROL_TOKEN`. If no token is set, the device refuses LAN control and OTA. Set the token before you install this fork: on the setup page, or with `POST /api/v2/token`. If you installed the fork with no token, do a front-panel reset (Power+Auto) and connect to the setup AP.
2. **The setup AP starts only after a failed Wi-Fi join.** The release build sets the AP mode to "fallback". Upstream uses "always". A mode that you set on the setup page has priority.
3. **First-heat check.** Until a pass is stored, the first heat after each boot checks that the PTC sensor rises first. If the chamber sensor rises first, or if the PTC sensor does not rise, the heat stops and a fault latches. The firmware never swaps the sensors by itself. The thresholds are provisional, because no capture from real hardware has set them yet. See [SAFETY.md](SAFETY.md#first-heat-check-thermistor-roles-and-element).
4. **A hazard trip during a panic-off or an inhibit survives a power cycle.** Upstream keeps such a trip in RAM only.
5. **Stricter input checks.** For a request to `/settings` with a bad field, the firmware applies no field. A zone target above 255 is clamped, not wrapped. `/api/v2/presets` and `/api/v2/bambu/scan` need the auth header.
6. **A slow client cannot hold the web server.** A body read stops after 3 receive timeouts or 10 s. Socket timeouts are 3 s.
7. **New endpoints and settings:** `GET /metrics` (Prometheus), syslog over UDP and SNTP time (Setup, "Syslog and time"). All are off or read-only by default.

The [API documentation](api-v2.md) and [SAFETY.md](SAFETY.md) on this branch include these changes.

## Changes and their upstream status

"Draft" means that the issue or PR text is ready and not posted yet.

### Safety and security

| Change | Branch | Upstream |
| --- | --- | --- |
| A hazard trip on top of a RAM-only latch goes to NVS. A failed clear leaves flash latched. | `fix/heater-fault-persist-only` (PR), `fix/heater-fault-persist` (with the host test) | Draft |
| The setup portal closes its open routes after a late Wi-Fi join. The Wi-Fi scan needs auth in station mode. | dragon-core `fix/portal-late-join`, `fix/portal-scan-auth` | Draft |
| A stalled client cannot hold the web server. Bodies are read in full. | `fix/httpd-body-read`, `fix/httpd-timeouts` | Draft |
| The same fix for the portal and the OTA upload | dragon-core `fix/portal-body-read` | Draft |
| `/settings` checks all fields before it applies one. Zone targets are clamped. | `fix/httpd-settings-validate` | Draft |
| Auth on presets and the Bambu scan. Constant-time token compare. Optional "require token" and lockout. | `feat/auth-hardening` | Draft |
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
| Host test of the real `pb_heater.c` with randomized invariants | `test/heater-control-tick` | Offered with the fault-latch fix |
| libFuzzer for the HTTP handlers and the dragon-core parsers | `test/fuzz-http-handlers`, dragon-core `test/fuzz-parsers` | Not proposed yet |
| One host-test runner, sanitizer CI matrix, coverage | `ci/host-test-hardening` | Not proposed yet |
| Static checks on the ELF (calls from the control task, worst-case stack) | `ci/elf-static-checks` | Not proposed yet |
| `-Werror` on `db_*` and `main` | `build/werror-all-first-party` | Draft |
| `.idf-version` is the one place for the ESP-IDF version | `build/idf-version-file` | Not proposed yet |
| Release checksums list only the published files. The manifest lists all components. | `fix/release-assets` | Draft |
| SBOM (SPDX) in CI and releases, Dependabot for actions | `deps/sbom-dependabot` | Not proposed yet |
| `flash.py` and `hil.py` declare their dependencies (PEP 723). The flash backup is read in chunks. | `build/pep723-script-deps`, `fix/flash-read-chunked` | Not proposed yet |
| Smoke test, capture and replay tools | `feat/network-smoke-test`, `feat/capture-tools` | Not proposed yet |
| ESP-IDF v6.1 support (v5.3 support ends in January 2027) | `build/idf-v6`, dragon-core `deps/led-strip-3`, `build/idf-v6-core` | Draft (dragon-core issue first) |

### Fork-only

These commits stay in this fork. They are not for upstream.

- The release settings in [`sdkconfig.release`](../sdkconfig.release): require a token, AP mode "fallback".
- The pins to the dragon-core fork and `dependencies.lock` in git.
- The CI trigger for the branch `fork`. The Pages deploy runs only upstream.

## Install

Use the steps in the [README](../README.md#install--update), with these changes:

1. Set a control token first (see item 1 above).
2. Download the image from the [releases of this fork](https://github.com/anyasabo/DragonBreath/releases), or build it from the branch `fork` with the release settings:

   ```bash
   ./tools/idf-build.sh . esp32c3 build-release -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release" build
   # The image is build-release/dragonbreath.bin.
   ```

3. After the install, run the smoke test: `python3 tools/smoke.py HOST --token TOKEN` ([SMOKE_TEST.md](SMOKE_TEST.md)).

To go back to upstream, install an upstream release image through `/fw`. The fork stores some settings in NVS that upstream ignores. A front-panel reset erases them.
