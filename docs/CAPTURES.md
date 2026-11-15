# Captures: record, analyze, replay

These tools turn a recording of the real heater into firmware numbers and
regression tests. The numbers are first-heat limits, sensor agreement, airflow
baselines and PID gains. All tools use only the Python 3 standard
library. They run on any computer on the heater LAN.

| Tool | What it does |
|---|---|
| `tools/capture.py` | Records a named scenario into one capture directory. Read-only by default. |
| `tools/analyze_capture.py` | Computes the numbers from a capture. Writes `analysis.md` and `analysis.json`. |
| `tools/replay_capture.py` | Replays a capture through the heater decision model. Checks the safety invariants. |
| `tools/priors.py` | Prints the expected values before a measurement (physics and known data). |
| `tools/synth_capture.py` | Writes synthetic captures (normal and five faults) to test the tools. |
| `tools/ntc_model.py` | Python port of the `pb_ntc` conversion. |
| `tools/heater_model.py` | Python model of the bang-bang tick and the first-heat rule. It is not the firmware. |

`tools/diag.py` stays the simple logger for issue reports. `analyze_capture.py`
also reads its CSV.

## Safety

- A capture is read-only unless you give `--allow-control --token TOKEN`.
  Without them, the tool tells you what to press and waits for it.
- With control, the tool sends only `POST /api/v2/command` (`power_on` at the
  scenario target, `off`) and `POST /api/v2/heartbeat` for its own lease. It never
  changes a setting, a calibration offset or a limit. It never clears a fault.
- The target is 30–60 °C and not above the device `max`.
- The tool does not start heat in these conditions: a fault is latched, the mode
  is not `off`, a sensor is not `ok`, or the chamber is above target − 2 °C.
- The tool asks before `power_on`. `--yes` removes the question.
- While its `power_on` can be active, each exit path (end, error, Ctrl-C,
  SIGTERM, SIGHUP) sends OFF and checks that the relay is off within 3 s. If
  not, it prints "Switch the heater off at the mains NOW".
- If another controller takes the heater, the tool does not send OFF over it.
- The tool also stops its own heat in these conditions: a fault latches, the
  mode changes, the chamber is 10 °C above the target, or the PTC is at 104 °C
  for 20 s. The firmware limits stay the real protection (see `SAFETY.md`).

## Scenarios

```bash
python3 tools/capture.py HOST idle --ambient-c 21.5 --notes "cold, door closed"
python3 tools/capture.py HOST first-heat --ambient-c 21.5
python3 tools/capture.py HOST step --target 50 --then-cooldown
python3 tools/capture.py HOST cooldown
python3 tools/capture.py HOST soak --duration 14400
# let the tool start and stop the heat:
python3 tools/capture.py HOST first-heat --allow-control --token TOKEN
```

| Scenario | Default rate | Stops when |
|---|---|---|
| `idle` | 2 Hz, `/metrics` each 15 s | 30 min (`--duration`) |
| `first-heat` | 2 Hz | 60 s baseline, first relay ON, 120 s of heat, OFF, 60 s tail |
| `step` | 1 Hz | target reached + `--hold` (30 min), or 2.5 h (`--duration`) |
| `cooldown` | 1 Hz | fan off, relay off, PTC and chamber within 3 °C for 60 s, or 1 h |
| `soak` | 0.1 Hz, `/metrics` each 60 s | 4 h (`--duration`) |

The `step` default of 2.5 h is about three time constants of an enclosed
printer. A CORE One L cools with a median time constant of 47 min. A shorter
step gives an uncertain time constant and gain.

## Capture format

One directory `captures/YYYYmmddTHHMMSSZ-SCENARIO/`:

- `header.json`: `/api/v2/info` (firmware, `rref_kohm`), `/settings`,
  `/api/v2/calibration`, `/api/v2/heater_method`, `/api/v2/health`, host time,
  scenario, notes, `ambient_c`, mains voltage, the stop reason.
- `state.csv`: one row for each `/api/v2/state`. The columns are in
  `tools/dbcapture_common.py` (`STATE_COLUMNS`). A field that the firmware does
  not send stays empty, for example `first_heat` before `feat/first-heat-check`.
- `state.jsonl.gz`: the raw JSON of each state, for fields added later.
- `metrics/`: each `/metrics` answer. A firmware without `/metrics` gives a note.
- `events.csv`: prompts, commands, reboots and errors.

Each write is flushed. An interrupted capture is still usable.

## Analysis

```bash
python3 tools/analyze_capture.py captures/DIR --ambient-c 21.5 --mains-v 230 --rated-v 230
```

The report starts with the flags and a table "Numbers for the firmware". Then
it gives the priors (expected values), then the measured values:

- **First heat:** the PTC and chamber rise at 12 s and 30 s of relay ON time,
  the provisional rule of `feat/first-heat-check` on the whole window, and
  suggested values for `PB_FH_PASS_RISE_DC` and `PB_FH_CROSS_RISE_DC` with
  their margins.
- **Cold agreement:** PTC − chamber after 30 min or more with no heat and the
  fan off. The readings with the other Rref, and the thermistor-chain
  hypotheses that fit the resistance ratio (`tools/priors.py`).
- **Airflow:** the outlet/PTC rise ratio at each heat start, the foldback cycles
  for each hour of heat, and the PTC cooldown time constant with the fan on.
- **Step response:** a first-order-plus-dead-time fit with the relay as input,
  SIMC PI gains (Skogestad) and Cohen-Coon PID gains in the firmware form
  (duty 0–1 per °C), and the overshoot and settling of the recorded control.
- **Control health:** late ticks, tick times, heap and stack minima.
- **Replay:** see below.

Each section uses only the columns that exist. One capture is one board and
one day: before you change a constant, compare several captures.

## Replay and regression tests

`tools/replay_capture.py CAPTURE` checks the safety invariants. The relay must
be OFF in these conditions: a fault, not armed, PTC ≥ 105 °C, chamber ≥ 85 °C,
a bad sensor. The tool also compares each bang-bang relay decision with
`tools/heater_model.py`. The state API reports a 5-sample average, so samples
near a threshold or a transition are "ambiguous", not failures.

To make a real capture a regression test:

1. Copy the capture to `tests/captures/NAME/`. If `state.jsonl.gz` is large, remove it.
2. Run `python3 tools/analyze_capture.py tests/captures/NAME --write-expect`.
3. Read `expect.json` and keep only the checks that must stay true.
4. Run `python3 tools/replay_capture.py tests/captures/NAME --expect tests/captures/NAME/expect.json`.

`tests/test_capture_tools.py` runs this replay for each directory in
`tests/captures/`.

### C replay (after the heater host environment is merged)

The Python model is a copy of the rules. The C host test environment is on
`test/heater-control-tick` (`tests/heater_host_env.c` and the ADC stubs of
`tests/pb_ntc_chain_host_test.c`). After that branch is merged, the firmware
code itself can replay a capture:

1. `python3 tools/replay_capture.py CAPTURE --export-adc adc.csv` writes one line
   for each 0.5 s tick: `t_ms,chamber_mv,ptc_mv,target_c,relay_recorded`. The
   millivolts give the recorded temperature with the board Rref
   (`ntc_model.temp_to_mv`).
2. A host test sets `env->adc_mv[]` and the target from each line, calls
   `pb_heater_tick()`, and compares the SSR stub with `relay_recorded` with the
   same "ambiguous" rule.
