# Network smoke test

`tools/smoke.py` is an attended release check over API v2. The release image has
no serial console. Thus, after each OTA install through `/fw`, use this tool on the
real heater. It uses only the Python 3 standard library.

```bash
python3 tools/smoke.py dragonbreath.local --token YOUR_TOKEN
python3 tools/smoke.py 192.168.1.50 --steps 1,2      # only some steps
```

If no control token is set on the device, `--token` can be any value. The
default is `$DRAGONBREATH_TOKEN`, or `smoke`. Step 1 checks the token with the
auth-gated, read-only `GET /api/v2/console`. A wrong token gives "wrong --token".

## Safety

- Stay at the heater during the test. Steps 2, 3 and 5 energize the real heater.
- The tool asks before each step that heats or changes a setting. If you do not
  type `y`, the step is SKIP.
- The heat target is 40 °C by default. The tool refuses a target above 50 °C or
  above the `max` value of the device.
- A heating step refuses to start in these conditions: a fault is latched, the
  mode is not `off`, a sensor is not `ok`, or the chamber is above target − 2 °C.
- `power_on` uses the revision of this check. If the state changes before
  `power_on` (for example, a button arms AUTO), the device rejects it. Then the
  tool sends nothing more, and the other mode continues.
- The tool sends the lease heartbeat from its main loop. If the tool stops, the
  lease expires and the comms watchdog stops the heater.
- After the tool sends `power_on`, the tool is "armed". While it is armed, each
  exit path sends OFF and checks that the relay is off and the mode is `off`.
  This includes a failure, an exception, Ctrl-C, SIGTERM and SIGHUP.
- Before this OFF, the tool reads the state. If another controller has the heater
  now (the mode is not `off` and the lease is not from `smoke-test`), the tool
  does not send OFF. It tells you to watch the heater and records a FAIL. If the
  state does not come, the tool sends OFF.
- Step 3 records the `comms_ms` restore before it changes the value. The restore
  runs at the end of step 3, also after an interrupt. If it does not complete,
  the summary has a FAIL row with the value to set.
- If the relay and the mode are not off 3 s after OFF (`--off-verify`), the tool prints
  "Switch the heater off at the mains NOW" and records a FAIL.
- Ctrl-C stops the run. The tool does not ask for the next step.
- At the end, the tool prints the device state and a summary. Each FAIL has an
  action line.

## Steps

| Step | What it does | PASS if |
|---|---|---|
| 1 read-only | Checks the token. `GET /api/v2/state`, `/api/v2/health`, `/metrics`. Prints firmware, reset reason (if the firmware reports it), sensors, mode, source and fault. | Both sensors are `ok`, no fault is latched (or `--allow-fault`). If `/metrics` exists, the required families are in it and `dragonbreath_fault_latched` agrees with the state. A 404 on `/metrics` is a note. |
| 2 heat on/off | `power_on` at the target, heartbeat the lease, wait for `heater.output` true, then `off`. | The relay goes on in `--heat-timeout` (30 s). The relay is off and the mode is `off` in `--off-deadline` (1.0 s, two control ticks) after OFF. |
| 3 comms watchdog | Sets `comms_ms` to `comms_ms_min` (10 s), `power_on`, waits for the relay and one accepted heartbeat, then stops the heartbeat. Restores `comms_ms` at the end, with retries. | A fault latches with a lease-expired or link-lost reason, the relay is off and the mode is `off`. The trip time from the last accepted heartbeat is between the timeout and the timeout + `--trip-margin` (2 s). |
| 4 persistence | Needs a latched fault. You power-cycle the heater, or type `r` to send `POST /api/v2/restart`. Then `clear_fault`. | `boot_id` changes, the fault is still latched, the mode is `off`. After `clear_fault` and 1.5 s, the fault is not latched. |
| 5 panic-off | `power_on` and heartbeat. When the relay is on, you hold a front-panel button for 2 s. | The mode changes to `off` with source `button`, the relay is off, and the old lease gets `stale_lease`. The tool then clears the RAM-only panic-off latch. |

The tool clears only a fault that this run caused: the step 3 trip or the step 5
panic-off. After a reboot, the reason of the step 3 trip is "safety trip". For
each other fault, step 4 shows the reason and asks before it clears the fault.
If step 3 runs without step 4, the tool clears the step 3 fault at the end.

`--allow-fault` lets step 1 pass with a latched fault. Use it only to test the
persistence of an earlier fault: `--steps 1,4 --allow-fault`. The heating steps
still refuse to start while a fault is latched.

If step 1 fails, the tool does not run the other steps. If a step fails or if
you interrupt the test, the exit code is 1.

## If a step fails

1. If the tool tells you to switch off the mains, do that first.
2. Do not keep the firmware. Roll back: install the previous release on `/fw`,
   or use **Boot inactive slot** (`POST /api/v2/boot-inactive`).
3. Open the dashboard Safety settings. Make sure that `comms_ms` has your usual
   value. Step 3 sets it to the minimum for a short time.
4. If a fault stays latched, read its reason in the final device state. When you
   know the cause, clear the fault on the dashboard.
5. Keep the full output of the tool for the bug report.

A failure in step 1 is usually a wrong token, a network problem or a sensor
problem. Read the action line.

## Tests without hardware

`tests/test_smoke.py` runs the tool end to end against a mock API v2 server
(`http.server` in a thread). The mock follows `pb_httpd` and `pb_policy`. It has
the auth header, request IDs, expected revisions and 32-character leases. The
lease TTL is `comms_ms` (minimum 10 s) and the control tick is 500 ms. It also has
the latched watchdog trip, the persisted fault over a reboot (the server is
offline for 2 s), panic-off and the `clear_fault` errors. CI runs it:

```bash
python3 -m unittest tests/test_smoke.py
```
