# DragonBreath API v2

API v2 is the sole device control contract. It replaces the alpha `/status`,
`/target`, `/heartbeat`, and `/reset` routes; clients must not probe for or fall
back to those routes.

All bodies are JSON. Read-only routes are open on the trusted LAN. Mutating
routes require `X-DragonBreath-Auth`; the device does not enable CORS.

## Read-only routes

- `GET /api/v2/info` — API version, stable device ID, per-boot ID, firmware,
  capabilities, `rref_kohm` (the resolved thermistor reference resistor — 82 on
  V1.0.1, 33 on V1.0 — read once at boot from the strap), and `inactive_slot`
  (`{project, version, label}` of the other OTA slot, or `null` if empty/unreadable
  — powers the "boot inactive slot" revert/rollback action).
- `GET /api/v2/state` — full authoritative snapshot. Reading never refreshes a
  lease or otherwise changes device state.
- `GET /api/v2/events` — Server-Sent Events. A `state` event is sent on connect
  and when `state_revision` changes. A full `telemetry` snapshot is sent every
  two seconds between transitions. At most two streams are held concurrently.
- `GET /api/v2/health` — uptime, heap, Wi-Fi RSSI/channel and SSE client count.
- `GET /api/v2/logs` — the in-memory event ring (newest first) as
  `{"api_version":2,"count":N,"entries":[{"ms":<millis since boot>,"text":"…"}]}`.
  Open (read-only, no side effects).
- `GET /api/v2/console` — **auth-gated** (`X-DragonBreath-Auth`) `text/plain` snapshot
  of the firmware `ESP_LOGx` byte ring (boot log included), backing the `/console`
  page. The one GET that requires the header: raw logs are chatty and not
  world-readable. No side effects.

`GET /api/v2/info` also returns `project` (the running app's project name —
`dragonbreath`, or `panda_breath` on a reverted slot). The **configured control
source** is reported on `/api/v2/state` (`environment.control_source`), not here.

`GET /api/v2/info` additionally returns a `ui` object —
`{"schema":1,"product":"dragonbreath","display_name":"DragonBreath"}` — that lets
the shared `dc_ui` single-page dashboard (vendored from dragon-core) identify the
product and adapt its capability gating. It is additive; firmware that predates it
is handled by the SPA's compatibility fallback.

The complete snapshot, not locally remembered intent, is the source of truth:

```json
{
  "api_version": 2,
  "device_id": "dragonbreath-a1b2c3",
  "boot_id": "4f7f00004f7f00004f7f00004f7f0000",
  "firmware": "0.3.0",
  "state_revision": 12,
  "mode": "power_on",
  "source": "klipper",
  "target": {
    "requested_c": 45.0,
    "effective_c": 45.0,
    "maximum_c": 70.0
  },
  "config": {
    "max": 70.0,
    "max_abs": 70.0,
    "comms_ms": 300000
  },
  "heater": {
    "demand": true,
    "output": true,
    "commanded_duty": 0.700,
    "approach_limit": 0.700,
    "constraint": "approach_limit",
    "method": "bangbang"
  },
  "fan": {
    "requested_percent": 100,
    "effective_percent": 100,
    "reason": "heater"
  },
  "sensors": {
    "chamber": {"temperature_c": 36.2, "status": "ok"},
    "ptc": {"temperature_c": 73.1, "status": "ok"}
  },
  "environment": {
    "control_source": "bambu",
    "bambu_printing": true,
    "bambu_filament": "ASA",
    "printer_chamber_temperature_c": 42.3,
    "printer_chamber_age_ms": 380,
    "moonraker_connected": true,
    "bed_temperature_c": 100.0,
    "bed_target_c": 100.0,
    "auto_engaged": false,
    "auto_filtering": false,
    "auto_bed_threshold_c": 0.0
  },
  "drying": {"active": false, "remaining_seconds": 0},
  "params": {
    "manual_target_c": 50.0,
    "auto_target_c": 60.0,
    "auto_bed_threshold_c": 100.0,
    "dry_target_c": 60.0,
    "dry_hours": 12,
    "filter_temp_c": 30.0,
    "filter_auto_enable": true
  },
  "control": {
    "lease": {
      "active": true,
      "owner": "u1-klippy",
      "expires_in_ms": 299500
    }
  },
  "first_heat": {
    "state": "pass",
    "ptc_rise_c": 22.0,
    "ch_rise_c": 2.2,
    "on_ms": 12000,
    "stored": "pass",
    "stock_hint": "none",
    "restarts": 0,
    "skipped": 0,
    "uncertain_streak": 0,
    "enforced": true,
    "advice": null
  },
  "safety": {
    "fault_latched": false,
    "inhibited": false,
    "reason": null
  }
}
```

`first_heat` reports the first-heat check (see `docs/SAFETY.md`). `state` is the
state of this session: `idle`, `armed`, `eval`, `pass`, `wrong_roles`, `no_rise`
or `uncertain`. `uncertain` means that both channels rose by similar amounts. It
reports only and does not latch. The next heat runs the check again.
In `armed`, a window is open with less than 12 s of relay ON time. In `eval`, a
window is open and a decision can come in each tick. `ptc_rise_c` and
`ch_rise_c` are the rises of the open or last window in °C (one decimal). After
a boot they are the stored rises. `on_ms` is the relay ON time of the open or
last window. `stored` is the result in NVS: `unknown`, `pass`, `wrong_roles` or
`no_rise`. A stored result counts only for the thermistor mapping and offsets
that it was measured with. `stock_hint` is the stock firmware's `ntc_bit` key:
`none` (no key), `unknown`, `crossed`, `normal` or `invalid`. It is a hint only.
`restarts` counts the windows of this boot that closed without a result (too
little ON time, or an offset change). `skipped` counts the relay-on edges of
this boot with an element that was already warm (no window).
`uncertain_streak` counts the consecutive `uncertain` results of this boot.
Another result sets it to 0. A value that stays above 0 over many heats can
mean a PTC sensor that is not on the element (see `docs/SAFETY.md`).
`enforced` is `false` on a safe dev-board build and on a build with
`CONFIG_PB_FIRST_HEAT_REPORT_ONLY=y`: the check then reports only and never latches.
A `wrong_roles` or `no_rise` result latches the heater off with the
`safety.reason` `sensor roles wrong or PTC sensor not on element (first-heat check)`
or `heater no-rise (first-heat check)`. If a `wrong_roles` result is stored,
`advice` tells the user to check the plugs before a swap. Else it is `null`.

`heater.commanded_duty` is the normalized duty admitted to the 10-second SSR
window after the PID approach policy and non-latching thermal governors. It is
not phase-angle modulation; `heater.output` remains the instantaneous on/off SSR
command. `heater.approach_limit` reports the product-owned duty ceiling selected
for the current control error. `heater.constraint` is one of `off`, `none`,
`approach_limit`, `target_reached`, `local_foldback`, `element_foldback`,
`pid_error`, or `unknown`. `approach_limit` means that the soft approach cap is
active (`heater.approach_limit` < 1.0) and no higher-priority constraint applies.

`heater.method` is the active chamber-control method: `bangbang` (the v1.1.15
hysteresis on/off drive, the **default**) or `pid` (the v1.1.16 dc_pid path with
the near-target approach cap). Both retain every safety layer (over-temp/sensor/
comms trips and the local + element foldback governors); only the duty decision
differs. In bang-bang, `commanded_duty` is 1.0 or 0.0 and `approach_limit` is 1.0.

Select it with `GET`/`POST /api/v2/heater_method`:

```
GET  /api/v2/heater_method   -> { "api_version": 2, "method": "bangbang" }
POST /api/v2/heater_method   { "method": "pid" }      (auth-gated; applies live, no reboot)
```

`method` must be `"bangbang"` or `"pid"`; the choice is persisted in NVS. This is
deliberately a v2 route rather than part of the legacy `/settings` surface.

Temperatures are JSON `null` when their sensor status is not `ok`. Public
state and SSE snapshots intentionally omit the raw lease ID; only the
authenticated response that creates a POWER_ON lease returns it.
`state_revision` changes only for control, mode, lease, fault, and safety
transitions; ordinary temperature samples and successful heartbeats do not
increment it.

`environment.control_source` is the configured control binding — for example
`klipper`, `bambu`, `prusa`, or `ha` — chosen on `/setup` (distinct from the top-level `source`, which is
the last actor that changed state). The printer's configured Bambu serial is intentionally
not exposed by the public state API; it remains internal to the Bambu MQTT connection.
`environment.bed_target_c` is the **commanded** bed setpoint from the active source and
drives the fan-only filtration band.

When the active source is Bambu, `printer_chamber_temperature_c` exposes the printer's
own chamber sensor used for chamber set-point regulation. It is `null` when that sample is
unavailable or has aged out in `dc_bambu`; `printer_chamber_age_ms` is likewise `null`
when no usable sample exists. The API fields are read-only diagnostics. DragonBreath's
local chamber NTC and PTC remain safety-authoritative; external-sensor regulation is also
bounded by the local chamber thermal limiter before the independent hard over-temperature
cutoff.

`fan.reason` is one of: `off`, `heater` (airflow while heating), `thermal_purge`
(residual-heat cooldown purge), `auto_filter` (the fan-only filtration band),
`requested` (the manual filtration fan), or `fault` (safety airflow).
`environment.auto_filtering` is `true` while the fan-only filtration band is driving
the blower — whenever `filter_auto` is enabled, Moonraker is connected, and the bed
**setpoint** is at/above `filter_temp_c`. This is a **standing** band, independent of
mode (it runs even while idle). AUTO heat engagement is separate: every source
(Klipper, Bambu, PrusaLink) uses the active print's filament-zone target.

`params` reports the *remembered* mode parameters — the values most recently
accepted for each mode, used to pre-fill the UI and to re-arm a mode when the
caller supplies no values of its own (the front-panel buttons). These are the
only policy values that survive a reboot; the active mode, target, deadline, and
lease deliberately do not. Updating them is a side effect of an accepted
`power_on` / `auto` / `drying_start` command, and the value recorded is always
the clamped one the device actually applied — there is no separate write path.

## Commands

`POST /api/v2/command` takes a request ID, the revision observed by the caller,
actor identity, and one command:

```json
{
  "api_version": 2,
  "request_id": "client-generated-unique-id",
  "expected_revision": 12,
  "actor": {"kind": "klipper", "id": "u1-klippy"},
  "command": {"name": "power_on", "target_c": 45}
}
```

Supported commands:

- `{"name":"off"}` — unconditional. A missing or stale revision never blocks a
  safer OFF.
- `{"name":"power_on","target_c":45}` — creates a new device-issued remote
  lease and invalidates any previous lease.
- `{"name":"auto","target_c":45,"bed_threshold_c":60}` — arms device AUTO.
  Filament-aware sources use the active filament-zone target; bed-follow sources
  use `target_c` once the observed bed setpoint reaches `bed_threshold_c`.
- `{"name":"drying_start","target_c":50,"hours":4}` — bounded to 1–12 hours.
- `{"name":"drying_stop"}` — unconditional safer stop.
- `{"name":"filter","percent":100}` — manual fan-only filtration blower (0 = off,
  1–100 = on; the blower is on/off, so any non-zero runs it). Heater untouched;
  independent of mode and revision-independent. **Enable is idle-only**: turning it
  *on* while the heater is heating or the cooldown purge is running is rejected with
  `heater_busy` (HTTP 409); turning it *off* is always allowed.
- `{"name":"clear_fault"}` — clears a recoverable fault only at the expected
  revision. Permanent inhibits require correcting the condition and rebooting.

`actor.kind` is `web` or `klipper`; `actor.id` is at most 31 characters.
Successful responses contain `ok`, the `request_id`, and the complete new
`state`. A successful `power_on` response also contains a top-level `lease_id`.
Clients must take a POWER_ON lease only from that authenticated response, never
from public state/events and never invent or reuse one.

The device keeps a small replay cache keyed by `(actor.id, request_id)`.
Repeating the same request while its resulting revision is still current
returns the original response. Reusing it after state changed returns
`revision_conflict`. Safer `off` and `drying_stop` commands are never cached and
always execute.

## Lease heartbeat

```http
POST /api/v2/heartbeat
X-DragonBreath-Auth: ...
Content-Type: application/json

{"api_version":2,"lease_id":"exact 32-character device-issued ID"}
```

Only the currently active lease is accepted. A stale, superseded, expired, or
invented ID returns HTTP 409 `stale_lease`. Heartbeats extend the lease deadline
but do not change `state_revision`. Passive dashboards and reconnected clients
must not heartbeat a lease they did not receive from their own accepted
POWER_ON response.

## Errors

Errors are machine-readable and normally include the current authoritative
state:

```json
{
  "ok": false,
  "error": "revision_conflict",
  "message": "command rejected by authoritative policy",
  "request_id": "...",
  "state": {}
}
```

Defined error codes include `invalid_command`, `unsupported_api_version`,
`auth_failed`, `revision_conflict`, `stale_lease`, `fault_latched`,
`inhibited`, `heater_busy` (enabling filtration while the heater is heating/cooling —
HTTP 409), and `persist_failed` (an NVS write did not commit). Separately, `busy` is
the HTTP 503 returned by `GET /api/v2/events` when both SSE slots are already in use —
a stream-capacity limit, not a command rejection.

## Other endpoints (not versioned)

These predate/sit beside the versioned control surface and are stable:

- `GET /settings` — the runtime-configurable settings plus their bounds:
  `{"max","max_min","max_abs","comms_ms","comms_ms_min","comms_ms_max",`
  `"cool_release","cool_release_min","cool_release_max",`
  `"fb_cut","fb_cut_default","fb_cut_min","fb_cut_max",`
  `"filter_temp","filter_temp_min","filter_temp_max","filter_auto","leds_enabled",`
  `"swap_thermistors","swap_thermistors_active","first_heat_stored","first_heat_stock_hint"}`.
  Open (read-only, no side effects). `cool_release` is the cooldown-fan "cool down
  to" temperature (30–65 °C, default 40). `fb_cut` is the element-foldback cut
  (90–104 °C; `0` = auto → the per-Rref `fb_cut_default`). `filter_temp` (20–60 °C,
  default 30) + `filter_auto` (bool, **default off** — opt-in) drive the fan-only
  filtration band (standing — runs whenever enabled + Moonraker connected + bed
  setpoint ≥ `filter_temp`, independent of mode).
  `leds_enabled` is the status-LED master enable (bool, default `true`).
- `swap_thermistors` is the requested chamber/PTC input mapping for crossed JST
  connector assignments (bool, default
  `false`); `swap_thermistors_active` reports the mapping latched at boot. A
  changed selection is saved but requires a reboot to take effect.
  `first_heat_stored` and `first_heat_stock_hint` are the same values as
  `first_heat.stored` and `first_heat.stock_hint` in the state document.
- `POST /settings?max=<°C>&comms_ms=<ms>&cool_release=<°C>&fb_cut=<°C>&filter_temp=<°C>&filter_auto=<0|1>&leds_enabled=<0|1>&swap_thermistors=<0|1>&first_heat_rerun=1&first_heat_apply_swap=1` — update any subset.
  Auth-gated (`X-DragonBreath-Auth`). Safety values are clamped to the safe
  envelope server-side (the max-target ceiling can never exceed the absolute cap,
  the comms watchdog stays within its min/max) and the clamped effective values
  are echoed back. `leds_enabled=0` turns the four front-panel LEDs off (monitoring
  and safety are unaffected). The fixed 105 °C PTC / 85 °C chamber cutoffs are
  **not** settable.
  `swap_thermistors` exchanges the two logical sensor inputs and their cutoff
  roles; a changed mapping is applied after reboot.
  `first_heat_rerun=1` forgets the stored first-heat result. The next heat runs
  the check again. If the reset cannot be saved, the response is HTTP 500, but
  the check still runs again in this session.
  Only a relay OFF→ON edge with a cold element opens a window. Thus a re-run
  during a continuous heat has no effect until the relay turns off and on
  again. An edge with a warm element (PTC ≥ 70 °C, or ≥ the chamber + 10 °C) is
  skipped. For a clean re-run, let the element cool, then start a new heat.
  `first_heat_apply_swap=1` stages the swap for a stored `wrong_roles` result:
  the opposite of the active mapping. Check the plugs first. If the chamber and
  PTC plugs are crossed, the swap is right. A `wrong_roles` result can also
  mean a PTC sensor that is not on the element. A swap does not repair that. The
  swap applies after a reboot. The reboot also resets the stored result, so the
  next heat checks the new mapping. Without a stored `wrong_roles` result the
  response is HTTP 409. The firmware never applies a swap by itself.
- `GET /api/v2/calibration` — the per-channel temperature calibration offsets plus
  their bounds and the live readings:
  `{"api_version":2,"chamber_offset_c","ptc_offset_c","min":-5,"max":5,"chamber_c","ptc_c","chamber_raw_c","ptc_raw_c"}`.
  Open (read-only). `*_c` are the calibrated readings; `*_raw_c` are the
  pre-offset readings.
- `POST /api/v2/calibration` — auth-gated. Body
  `{"chamber_offset_c":<°C>,"ptc_offset_c":<°C>}` (either optional; at least one
  required). Each offset is **hard-clamped to ±5 °C** and persisted; the clamped
  effective offsets are echoed back. The offset is applied to the reading used by
  the display, control, AUTO **and** the over-temp cutoffs alike — so the fixed
  cutoffs shift by at most 5 °C and calibration can never disable a fault
  (see `docs/SAFETY.md`).
- `POST /api/v2/restart` — authenticated soft reboot. **Refused while the heater
  is armed/on** (HTTP 409 `heater_active`). Returns `{"ok":true}` then reboots
  once the response has flushed.
- `POST /api/v2/boot-inactive` — authenticated. Sets the **inactive OTA slot** as the
  boot partition and reboots into it — revert to stock (if that slot holds a
  `panda_breath` image) or roll back to a previous DragonBreath version. Only moves
  the boot pointer; **no flash write.** **Refused while the heater is armed/on** and
  returns `409 empty_slot` if the inactive slot has no bootable image. See
  `inactive_slot` on `/info` for what will boot.
- `POST /api/v2/factory-reset` — authenticated. **Refused while heating.** Requires
  explicit confirmation `confirm=factory-reset` (query or urlencoded body) or it
  returns HTTP 400 `confirmation_required`. On confirm it erases the whole
  `app_nvs` namespace (Wi-Fi credentials, Moonraker host, control token, safety
  limits), returns `{"ok":true}`, and reboots — the device comes back up in AP
  provisioning mode.
- `POST /api/v2/token` — authenticated. Body `{"token":"<=64 chars>"}` sets the
  control token; `{"token":""}` / `{"token":null}` / `{}` clears it. The secret is
  never echoed; the response is `{"ok":true,"token_set":<bool>}`. Once a token is
  set, every mutating request must carry it in `X-DragonBreath-Auth`.
- `POST /update` — authenticated application-image OTA. Streams the `.bin` into the
  inactive slot, verifies it (image checksum + project identity — accepts a
  `dragonbreath` image **or** a stock `panda_breath` image so a user can revert to
  stock over WiFi; all other images are rejected), reports the SHA-256 it computed,
  sets it as the boot slot, and reboots. **Refused while the heater is armed/on.** A
  bad image that crashes before the app marks itself healthy rolls back on the next
  boot.

The device also serves the human web UI (not part of the machine control contract):
the dashboard on `GET /`, plus `/setup` (Wi-Fi + control-source selector), `/fw` (OTA),
`/diag` (live diagnostics telemetry + CSV), `/console` (firmware log viewer),
`/favicon.ico`, `/scan.json`, and `POST /save`, `/rescan`.
