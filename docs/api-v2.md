# DragonBreath API v2

API v2 is the sole device control contract. It replaces the alpha `/status`,
`/target`, `/heartbeat`, and `/reset` routes; clients must not probe for or fall
back to those routes.

All bodies are JSON. Read-only routes are open on the trusted LAN. The routes in
"Checked routes" below require `X-DragonBreath-Auth`. The device does not enable CORS.

## Authentication

### Checked routes

These routes check the `X-DragonBreath-Auth` header:

- API v2 (`pb_httpd`): `POST /api/v2/command`, `/api/v2/heartbeat`,
  `/api/v2/token`, `/api/v2/calibration`, `/api/v2/zones`,
  `/api/v2/heater_method`, `/api/v2/restart`, `/api/v2/boot-inactive`,
  `/api/v2/factory-reset`, `POST /settings` and `GET /api/v2/console`.
- Product routes (`db_portal`): `POST /api/v2/presets` and
  `POST /api/v2/bambu/scan`.
- Shared portal routes (dragon-core `dc_portal`): `POST /api/v1/provisioning/ap`,
  `POST /api/v1/provisioning/product`, `GET /api/v1/system/logs`,
  `GET /api/v1/system/console`, `POST /api/v1/system/update`, `POST /update` and
  `POST /api/v1/system/reset`.

These routes do **not** check the header:

- `GET /api/v1/provisioning` and `POST /api/v1/provisioning/wifi`: if the device
  started in the setup portal (no saved Wi-Fi credentials, or a failed join at
  boot), they are open. This is by design: a new device has no token. In station mode, they check
  the header.
- `POST /api/v1/provisioning/scan` (a Wi-Fi scan). This is a gap in dragon-core.
- All other `GET` routes: the pages, `/api/v2/info`, `/state`, `/events`, `/health`,
  `/logs`, `/calibration`, `/zones`, `/heater_method`, `/presets`,
  `/bambu/discovered`, `/km-config` and `/diag`.

### Rules

- **No control token set (default build):** a non-empty header passes. This is a
  CSRF gate, not authentication.
- **Control token set:** the header must equal the token. The device compares in
  constant time. Else it returns HTTP 403 `auth_failed`. A new token must have 1-64
  printable ASCII characters (0x21-0x7E), with no spaces.
- **`CONFIG_DB_REQUIRE_CONTROL_TOKEN=y` and no token set:** the device returns HTTP 403
  `token_required`, except for one case. If the request arrived on the setup AP
  address and the Wi-Fi station is not associated, a non-empty header passes. Then a setup AP client can set the first token with
  `POST /api/v2/token` or the setup form field `ctl_token`. In this build,
  `POST /api/v2/token` refuses to clear a token (HTTP 400 `token_required`).
- **`CONFIG_DB_AUTH_LOCKOUT=y`:** after `CONFIG_DB_AUTH_LOCKOUT_MAX_FAILS` wrong
  tokens (default 10) in `CONFIG_DB_AUTH_LOCKOUT_WINDOW_S` (default 60 s), every
  check fails for `CONFIG_DB_AUTH_LOCKOUT_DURATION_S` (default 60 s). The window
  starts at the first wrong token. The correct token also fails during a lockout.
  A missing header does not count as a wrong token.

### Error responses

- The API v2 and product routes return HTTP 403 `auth_failed`, HTTP 403
  `token_required` or HTTP 429 `auth_locked`.
- The shared portal routes use a callback that returns only true or false. For
  each failure kind, also a lockout, they return HTTP 403 `authorization required`.
- On each HTTP 403, the dashboard forgets its stored token and asks for a token
  again.

**The lockout is a denial-of-service lever.** Each host that can reach the API can
send wrong tokens: other LAN hosts, and hosts on other VLANs that can route to the
device. During a lockout, OTA, settings and all HTTP controllers (for example the Klipper
helper) get errors. A remote heat lease then expires, and the
heater fails safe to OFF. The front-panel buttons continue to operate.

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
- `GET /metrics`: Prometheus text format 0.0.4 (not versioned). See
  [Prometheus metrics](#prometheus-metrics).
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
  "reset_reason": "poweron",
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
  "safety": {
    "fault_latched": false,
    "inhibited": false,
    "reason": null
  }
}
```

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
`reset_reason` is the `esp_reset_reason()` of this boot as a stable string.
It does not change until the next boot. The values are:

```text
unknown poweron ext sw panic int_wdt task_wdt wdt deepsleep
brownout sdio usb jtag efuse pwr_glitch cpu_lockup
```

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
`auth_failed`, `token_required` (HTTP 403), `auth_locked` (HTTP 429),
`revision_conflict`, `stale_lease`, `fault_latched`,
`inhibited`, `heater_busy` (enabling filtration while the heater is heating/cooling —
HTTP 409), and `persist_failed` (an NVS write did not commit). Separately, `busy` is
the HTTP 503 returned by `GET /api/v2/events` when both SSE slots are already in use —
a stream-capacity limit, not a command rejection.

## Prometheus metrics

`GET /metrics` returns the device telemetry in the Prometheus text format 0.0.4
(`Content-Type: text/plain; version=0.0.4`). The route is open and read-only,
like `GET /api/v2/state`. A scrape does not change device state. Prometheus or
VictoriaMetrics can scrape the device directly. Use an interval of 15–30 s.

How the device builds the response:

- It copies the policy snapshot once (the same lock as `/api/v2/state`), then
  reads the other counters with short critical sections.
- A pure writer (`components/pb_metrics`) formats the text with integer math
  only. It uses no `%f`, no cJSON and no allocation of its own.
- The writer streams the text through a 1 KB buffer on the HTTP task stack.
  The response uses chunked transfer encoding. A typical response is 8–10 KB.
  The worst case (24 task names, maximum values) is approximately 13 KB.
- The snapshot (about 1.1 KB) and the task list are on the heap only while
  the request runs.
- If the snapshot allocation fails, the response is HTTP 500. If the client
  closes the connection during the response, the device stops the response.

Rules:

- Temperatures are in °C with one decimal (tenths of a degree).
- If a sensor status is not `ok`, the temperature sample is not in the
  response. There is no `NaN`. `dragonbreath_sensor_status` tells the cause.
- If a value is not known (no filament zone, no Wi-Fi link), the sample is not
  in the response. The `# HELP` and `# TYPE` lines are always there.
- Counters (`_total`) start at 0 at each boot, except `dragonbreath_boots_total`.
- A state set (mode, sensor status) has one sample for each state. The current
  state is 1, the other states are 0.

| Metric | Type | Labels | Meaning |
|---|---|---|---|
| `dragonbreath_temperature_celsius` | gauge | `sensor` = `chamber`, `ptc` | Smoothed, calibrated temperature. |
| `dragonbreath_sensor_status` | gauge | `sensor`; `status` = `ok`, `short`, `open`, `uninit` | Thermistor status (state set). |
| `dragonbreath_heater_demand` | gauge | | 1 if a target is armed and no fault is latched. |
| `dragonbreath_heater_relay_on` | gauge | | 1 if the SSR output is on. |
| `dragonbreath_fan_percent` | gauge | | Effective blower level. |
| `dragonbreath_mode` | gauge | `mode` = `off`, `power_on`, `auto`, `drying` | Control mode (state set). |
| `dragonbreath_target_celsius` | gauge | | Effective heater target. 0 when off. |
| `dragonbreath_zone_target_celsius` | gauge | | Filament-zone target from the control source. No sample if no zone is known. |
| `dragonbreath_fault_latched` | gauge | | 1 if a safety fault or an inhibit is latched. |
| `dragonbreath_fault` | gauge | `cause` = `none`, `ptc_overtemp`, `chamber_overtemp`, `chamber_sensor`, `ptc_sensor`, `link_lost`, `panic_off`, `inhibited`, `emergency`, `nvs_unreadable` | Cause of the latch (state set). `none` is 1 if no fault is latched. |
| `dragonbreath_heater_inhibited` | gauge | | 1 if heat is inhibited until reboot. |
| `dragonbreath_source_connected` | gauge | | 1 if the control-source link is up. |
| `dragonbreath_safety_trips_total` | counter | `cause` = `ptc_overtemp`, `chamber_overtemp`, `chamber_sensor`, `ptc_sensor`, `link_lost`, `inhibited`, `emergency` | Trips since boot. Each cause counts once in a latch episode, also if the latch was already active. A fault clear starts a new episode. The boot restore of a persisted latch does not count. |
| `dragonbreath_panic_offs_total` | counter | | Front-panel panic-off requests since boot. |
| `dragonbreath_relay_on_seconds_total` | counter | | SSR on-time since boot. |
| `dragonbreath_control_ticks_total` | counter | | Control-loop iterations since boot (periodic and wake-up). |
| `dragonbreath_control_ticks_late_total` | counter | | Periodic ticks that started more than 250 ms after the scheduled time. |
| `dragonbreath_control_tick_duration_seconds` | gauge | | Duration of the last control tick (1 µs resolution). |
| `dragonbreath_control_tick_duration_max_seconds` | gauge | | Longest control tick since boot. |
| `dragonbreath_control_tick_interval_max_seconds` | gauge | | Longest time between the starts of two ticks since boot. The period is 0.5 s. |
| `dragonbreath_heap_free_bytes` | gauge | | Free heap. |
| `dragonbreath_heap_minimum_free_bytes` | gauge | | Lowest free heap since boot. |
| `dragonbreath_heap_largest_free_block_bytes` | gauge | | Largest free heap block (8-bit capable). |
| `dragonbreath_tasks` | gauge | | Number of FreeRTOS tasks. |
| `dragonbreath_task_instances` | gauge | `task` | Number of tasks with this name. |
| `dragonbreath_task_stack_free_minimum_bytes` | gauge | `task` | Stack high-water mark: the least free stack since start. The minimum of the tasks with this name. |
| `dragonbreath_task_cpu_seconds_total` | counter | `task` | CPU time from the FreeRTOS run-time stats. The sum of the tasks with this name. |
| `dragonbreath_uptime_seconds` | gauge | | Time since boot. |
| `dragonbreath_boots_total` | counter | | Boot count in NVS. It increments once at each boot. A factory reset or the Power + Auto reset sets it to 0. No sample if the NVS write failed. |
| `dragonbreath_reset_reason_info` | gauge | `reason` | `esp_reset_reason()` of this boot (the strings of `reset_reason` in `/api/v2/state`). Always 1. |
| `dragonbreath_build_info` | gauge | `version`, `idf_version` | Firmware and ESP-IDF version. Always 1. |
| `dragonbreath_wifi_rssi_dbm` | gauge | | RSSI of the station link. No sample if not connected. |
| `dragonbreath_http_open_sockets` | gauge | | Open client sockets of the HTTP server (`httpd_get_client_list`). |
| `dragonbreath_syslog_enabled` | gauge | | 1 if the device sends its log to a syslog server (`sl_host` is set and `dc_syslog` started). |
| `dragonbreath_syslog_sent_total` | counter | | Syslog datagrams that `sendto()` accepted. UDP has no acknowledgement, so the server can receive fewer. |
| `dragonbreath_syslog_dropped_total` | counter | | Log lines dropped because the syslog queue (16 lines) was full, for example during a burst of log lines or while the station has no address. |
| `dragonbreath_syslog_send_errors_total` | counter | | Failed `sendto()` calls and failed server address lookups. |
| `dragonbreath_syslog_queue_lines` | gauge | | Log lines in the syslog queue now. |
| `dragonbreath_time_synced` | gauge | | 1 after the first SNTP sync. It stays 1 until reboot. The wall clock gives only log timestamps. |

Syslog metrics: if syslog is off, the syslog samples are 0. The stack of the
sender task is in `dragonbreath_task_stack_free_minimum_bytes{task="dc_syslog"}`.

Control-tick timing: the control task records the start and the end of each
loop with `esp_timer_get_time()`. If a periodic tick starts more than 250 ms
(half a period) after its scheduled time, it is late. A wake-up by a notification
(an accepted command or a panic-off) is not a scheduled tick, so it is never
late. If `dragonbreath_control_ticks_late_total` increases, or the maximum
interval is much more than 500 ms, other work delays the control loop.

Per-task metrics: the `task` label is the FreeRTOS task name. Tasks with the
same name (for example two `db_sse` streams) share one series. The response
holds a maximum of 24 task names. `dragonbreath_tasks` gives the full count.
If the firmware has no FreeRTOS trace facility, the task families are not in
the response. Without the run-time stats, the CPU family is not there. The CPU
share of a task is the rate of its CPU time:

```promql
rate(dragonbreath_task_cpu_seconds_total[5m])
```

The single core gives a sum of 1 for all tasks (`IDLE` is the free CPU time).
If a task stops, its series stops. If one of two tasks with the same name
stops, the CPU counter of that name decreases. `rate()` sees this as a counter
reset.

Example scrape configuration (Prometheus, or VictoriaMetrics `vmagent` and
single-node `-promscrape.config`):

```yaml
scrape_configs:
  - job_name: dragonbreath
    scrape_interval: 15s
    scrape_timeout: 5s
    metrics_path: /metrics
    static_configs:
      - targets: ["dragonbreath.local:80"]   # a static DHCP address is more reliable
        labels:
          device: chamber-heater
```

Example alerts (PromQL):

```promql
increase(dragonbreath_control_ticks_late_total[10m]) > 0
dragonbreath_fault_latched == 1
dragonbreath_reset_reason_info{reason=~"panic|int_wdt|task_wdt|wdt|brownout"}
min(dragonbreath_task_stack_free_minimum_bytes) < 512
increase(dragonbreath_syslog_dropped_total[10m]) > 0
dragonbreath_syslog_enabled == 1 and dragonbreath_time_synced == 0
```

## Other endpoints (not versioned)

These predate/sit beside the versioned control surface and are stable:

- `GET /settings` — the runtime-configurable settings plus their bounds:
  `{"max","max_min","max_abs","comms_ms","comms_ms_min","comms_ms_max",`
  `"cool_release","cool_release_min","cool_release_max",`
  `"fb_cut","fb_cut_default","fb_cut_min","fb_cut_max",`
  `"filter_temp","filter_temp_min","filter_temp_max","filter_auto","leds_enabled",`
  `"swap_thermistors","swap_thermistors_active"}`.
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
- `POST /settings?max=<°C>&comms_ms=<ms>&cool_release=<°C>&fb_cut=<°C>&filter_temp=<°C>&filter_auto=<0|1>&leds_enabled=<0|1>&swap_thermistors=<0|1>` — update any subset.
  Auth-gated (`X-DragonBreath-Auth`). Safety values are clamped to the safe
  envelope server-side (the max-target ceiling can never exceed the absolute cap,
  the comms watchdog stays within its min/max) and the clamped effective values
  are echoed back. `leds_enabled=0` turns the four front-panel LEDs off (monitoring
  and safety are unaffected). The fixed 105 °C PTC / 85 °C chamber cutoffs are
  **not** settable.
  `swap_thermistors` exchanges the two logical sensor inputs and their cutoff
  roles; a changed mapping is applied after reboot.
- Syslog and NTP settings. They are fields of the product setup schema of
  `dc_portal`, in the section "Syslog and time" of `GET /api/v1/provisioning`
  (`product_setup`). To change them, send
  `POST /api/v1/provisioning/product` with the body
  `{"values":{"sl_host":"alloy.lan","sl_port":514,"ntp_host":"ntp.lan"}}`
  (auth-gated, each field optional):
  - `sl_host`: the syslog server, a host name or an IPv4 address (63 characters
    maximum). `""` (the default) turns syslog off.
  - `sl_port`: the UDP port of the syslog server, 1–65535 (default 514). An
    empty string keeps the saved port.
  - `ntp_host`: an NTP server, a host name or an IPv4 address. If it is set,
    SNTP uses only this server. `""` (the default) means the NTP server from
    DHCP (option 42).

  The device validates the fields before it saves any field. A host name has
  labels of letters, digits and hyphens, separated by dots. The device reads a
  text of only numbers as an address (`1.2.3` is 1.2.0.3). If all labels are
  numbers, the field must have 4 decimal octets 0–255 without leading zeros.
  If a field is not
  valid, the response is HTTP 400 and names the field. The values are in NVS
  (`app_nvs`: `sl_host`, `sl_port`, `ntp_host`). They apply after a restart.
  A factory reset erases them.

  If `sl_host` is set, the device sends its ESP-IDF log as RFC 5424 syslog over
  UDP: APP-NAME `dragonbreath`, HOSTNAME the device host name, PROCID the log
  tag, and the structured data `[dc@32473 seq="N" up="MS"]` (sequence number and
  uptime in ms). A gap in `seq` shows dropped lines. If `sl_host` or `ntp_host`
  is set, SNTP runs. The wall clock gives only the syslog TIMESTAMP. Before the
  first sync, TIMESTAMP is `-`. Nothing that is related to safety or control
  uses the wall clock. The device does not combine `ntp_host` with the DHCP
  server: a DHCP reply with option 42 replaces all SNTP servers. Syslog starts
  after the HTTP server. If the device is in the setup portal (no station in
  this boot), syslog does not start.

  DHCP-only limit: the device has its first lease before SNTP starts. With the
  current dragon-core, SNTP gets the server from DHCP only at the next DHCP
  renewal (often half the lease time) or reconnect. If the timestamps are
  important, set `ntp_host`.
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
  set, every mutating request must carry it in `X-DragonBreath-Auth`. With
  `CONFIG_DB_REQUIRE_CONTROL_TOKEN=y`, a clear returns HTTP 400 `token_required`.
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
