# Network smoke test

`tools/smoke.py` is an attended release check over API v2. The release image has
no serial console. Thus, after each OTA install through `/fw`, use this tool on the
real heater. It uses only the Python 3 standard library.

```bash
python3 tools/smoke.py dragonbreath.local --token YOUR_TOKEN
python3 tools/smoke.py 192.168.1.50 --steps 1,2      # only some steps
```

If no control token is set on the device, `--token` can be any value. The
default is `$DRAGONBREATH_TOKEN`, or `smoke`.

## Safety

- Stay at the heater during the test. Steps 2, 3 and 5 energize the real heater.
- The tool asks before each step that heats or changes a setting. If you do not
  type `y`, the step is SKIP.
- The heat target is 40 °C by default. The tool refuses a target above 50 °C.
- A heating step refuses to start in these conditions: a fault is latched, the
  mode is not `off`, a sensor is not `ok`, or the chamber is above target − 2 °C.
- Each heating step sends OFF in a `finally` block: on success, on a failure,
  on an exception and on Ctrl-C. The tool sends OFF again before it exits.
- If OFF cannot be sent, the tool tells you to switch off the mains.

## Steps

| Step | What it does | PASS if |
|---|---|---|
| 1 read-only | `GET /api/v2/state`, `/api/v2/health`, `/metrics`. Prints firmware, reset reason (if the firmware reports it), sensors, mode, source and fault. | Both sensors are `ok`, no fault is latched (or `--allow-fault`). If `/metrics` exists, the required families are in it and `dragonbreath_fault_latched` agrees with the state. A 404 on `/metrics` is a note. |
| 2 heat on/off | `power_on` at the target, heartbeat the lease, wait for `heater.output` true, then `off`. | The relay goes on in `--heat-timeout` (30 s). The relay is off in `--off-deadline` (1.0 s, two control ticks) after OFF. The mode is `off` and the lease is not active. |
| 3 comms watchdog | Sets `comms_ms` to `comms_ms_min` (10 s), `power_on`, heartbeat until the relay is on, then stops the heartbeat. Restores `comms_ms` in a `finally` block. | A fault latches with a lease-expired or link-lost reason, the relay is off and the mode is `off`. The trip time from the last accepted heartbeat is between the timeout and the timeout + `--trip-margin` (2 s). |
| 4 persistence | Needs a latched fault (from step 3). You power-cycle the heater, or type `r` to send `POST /api/v2/restart`. Then `clear_fault`. | `boot_id` changes, the fault is still latched, the mode is `off`. After `clear_fault` and 1.5 s, the fault is not latched. |
| 5 panic-off | `power_on` and heartbeat. When the relay is on, you hold a front-panel button for 2 s. | The mode changes to `off` with source `button`, the relay is off, and the old lease gets `stale_lease`. The tool then clears the RAM-only panic-off latch. |

If step 3 runs without step 4, the tool clears the step 3 fault at the end.
If step 1 fails, the tool does not run the other steps.

To test the persistence of an earlier trip, run `--steps 1,4 --allow-fault`.

Each step prints `PASS`, `FAIL` or `SKIP`. A summary follows. If a step fails or
if you interrupt the test, the exit code is 1.

## Tests without hardware

`tests/test_smoke.py` runs the tool end to end against a mock API v2 server
(`http.server` in a thread). The mock follows `pb_httpd` and `pb_policy`:
request IDs, expected revisions, 32-character leases, a lease TTL equal to
`comms_ms`, the latched watchdog trip, the persisted fault over a reboot and
panic-off. CI runs it:

```bash
python3 -m unittest tests/test_smoke.py
```
