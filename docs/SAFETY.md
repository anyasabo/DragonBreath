# Safety model

The Panda Breath heats a chamber with a **PTC ceramic element** switched by a
**solid-state relay**. Custom firmware controls that heater, so the safety story
matters. Good news: there are **two independent hardware backstops** that do not
depend on any firmware — this firmware is the third, soft layer.

## Layer 1 (hardware) — bonded thermal cutoff
A cylindrical thermal protector (bimetal KSD-type disc or one-shot thermal fuse)
is spliced **in series with the PTC's mains lead** and bonded to the heater
frame (confirmed on the real unit, photo IMG_7631). It sits **upstream of the
SSR**, so it opens on element over-temp **regardless of firmware and regardless
of the SSR failing shorted** — the classic worst-case failure.

## Layer 2 (physics) — PTC self-limiting
The element is a **PTC ceramic**: its resistance rises sharply at the Curie
temperature, collapsing its own power, so it is strongly **self-limiting** rather
than thermally runaway-prone. A firmware bug or a welded SSR should at most drive
the chamber toward the element's self-limited equilibrium, which the Layer-1
cutoff bounds. This is a strong physical safety margin, **not an absolute
guarantee** — no firmware can promise the absence of every fault.

## Layer 3 (this firmware) — soft cutoffs
`pb_heater` enforces, every control tick, before any heat request:
- **PTC element over-temp** → force off at 105 °C (stock parity). Always the first,
  unconditional, latching check.
- **Element foldback limiter** → below the 105 °C cutoff, the SSR is cut with
  hysteresis at a per-Rref point and held off until the element cools to a resume
  point, so a hot/slow element holds *under* the hard cutoff instead of ratcheting
  into it. Cut point is per-board (82 kΩ → 102 °C / 33 kΩ → 99 °C) or a user
  override `fb_cut` (90–104 °C). It can **only remove power**, can never exceed
  104 °C, and does not touch the fixed 105 °C hardware cutoff.
- **Chamber over-temp** → force off at 85 °C.
- **Sensor fault while heating** (open/short) → fail-closed.
- **Comms-loss watchdog** → if no controller link while heating for the watchdog
  timeout — **5 min by default, runtime-configurable to 10 s–5 min** (`comms_ms`,
  clamped to `[PB_HEATER_COMMS_TIMEOUT_MS_MIN, …_MAX]`; never disabled or beyond
  5 min) — latch off. (The device can't tell "idle hold" from "controller crashed",
  so a live link must be refreshed via `pb_heater_notify_link_alive()`.)
- **Set-point clamp** → target capped at the runtime `max` ceiling (default 70 °C),
  itself hard-bounded by `PB_HEATER_ABS_MAX_TARGET_C` (70 °C) — a target can never
  exceed the absolute cap regardless of settings.
- **Boot state** → SSR forced OFF in `pb_heater_init()` before anything else runs;
  a hung control loop trips the task watchdog (`CONFIG_ESP_TASK_WDT_PANIC`).

## Sensor calibration is bounded (cannot defeat a cutoff)
Each temperature channel (chamber, PTC) has a user-settable calibration
**offset** so a mis-reading sensor can be corrected. The offset is **hard-clamped
to ±5 °C** and the clamp is enforced **on every set AND on every NVS load** — a
corrupt or hand-edited stored value clamps to the bound, it is never applied raw.
The offset is applied inside `pb_ntc`, so the display, control regulation, AUTO,
and the over-temp cutoffs all act on the **same** calibrated value.

Because the offset is bounded to ±5 °C, the fixed cutoffs can shift by **at most
5 °C**: the chamber trips at **≥80 °C** worst case (85 − 5) and the PTC element at
**≥100 °C** worst case (105 − 5). The cutoff constants themselves
(`PB_HEATER_CHAMBER_MAX_C` = 85, `PB_HEATER_PTC_CUTOFF_C` = 105) are unchanged and
not user-configurable, and calibration touches only the reading, never the
sensor-fault fail-closed logic (which is raw-count based). Calibration therefore
**cannot disable a fault** or exceed the ±5 °C bound, and the Layer-1 bonded
thermal cutoff + Layer-2 PTC self-limit remain the true emergency layer regardless
of any offset.

## Thermistor channel mapping
`swap_thermistors` is an opt-in setting for the uncommon case where the chamber
and PTC sensor connectors are plugged into each other's JST sockets on the board.
It swaps the complete logical
channels, so the 85 °C chamber cutoff, 105 °C PTC cutoff, fault status, and
per-channel calibration follow the selected sensor roles. The choice is staged
in NVS and takes effect only after reboot; the live mapping cannot change while
the control loop is running. Leave it off unless crossed channels are confirmed,
and verify both readings on `/diag` before enabling heat. Reversing the two leads
of a single two-wire thermistor does not require this option.

## First-heat check (thermistor roles and element)
The stock firmware checks the first heat: if the chamber reading rises first,
it swaps the two channels silently. DragonBreath does a similar check, but it
never swaps. It latches the heater off and tells the user.

How it works (`pb_heater_tick()`, after the hazard checks above):
- A relay OFF→ON edge opens a window. If the element is already warm (PTC
  reading ≥ 70 °C, or ≥ the chamber reading + 10 °C), no window opens. Such an
  element can be at its equilibrium and cannot rise.
- The start reading of each channel is the median of its first three readings,
  so one glitch cannot make a false start.
- The window counts **relay ON time only**. The rise of each channel is its peak
  minus its start reading, in integer tenths of a degree. The peak uses the lower
  of each two consecutive readings, so one glitch sample is not a rise.
- From 12 s of ON time: PTC rise ≥ 3.0 °C and > 2 × chamber rise → **pass**.
- A failed result needs mostly continuous heat: only within 60 s of wall time
  after the open (a duty of 50 % or more). Short ON pulses at a low duty do not
  add up to a rise. In that time:
  - From 12 s of ON time: chamber rise ≥ 2 × PTC rise + 2.0 °C and chamber rise
    ≥ 10.0 °C → **wrong roles**.
  - At 30 s of ON time: chamber rise ≥ 2 × PTC rise + 2.0 °C → **wrong roles**.
    Else PTC rise < 2.0 °C → **no-rise** (dead element, open element circuit).
    Else → **uncertain** (both channels rose by similar amounts).
- A window without a result after 300 s of wall time closes (a restart, counted
  in `first_heat.restarts`). A stop, a fault latch, an offset change or any
  hazard trip also closes it. A hazard trip in the same tick wins.

Why "wrong roles" is "chamber rise ≥ 2 × PTC rise + 2 °C":
- The chamber thermistor sits in the heater's own outlet air. At full power that
  air can rise 75–150 K above the inlet (an estimate). With crossed leads the
  "PTC" channel reads that air and also rises fast, so an absolute limit on the
  PTC rise misses crossed leads.
- The outlet air cannot get hotter than the element, and it heats later. In a
  correct unit the PTC rise is larger than the chamber rise. A well-coupled PTC
  thermistor rises about 1.5–3 °C/s (a real trace: 1.44 °C/s).
- A chamber rise of twice the PTC rise plus a margin thus means crossed leads
  **or** a PTC thermistor that lags the element or is not on it. Both are faults:
  the PTC cutoff cannot see the element. The fault reason is neutral:
  `sensor roles wrong or PTC sensor not on element (first-heat check)`.
- When both rises are similar, the result is **uncertain**: it reports only
  (no latch, nothing stored), and the next heat checks again.

What a result does:
- **wrong roles** and **no-rise** latch the heater off with their own fault cause.
  The latch persists and clears like a hazard trip. After the clear, the next
  heat runs the check again.
- After **wrong roles**, check the plugs first. If the chamber and PTC plugs are
  crossed, apply the swap in Settings (`first_heat_apply_swap=1`, the existing
  `swap_thermistors`). Do not apply it in other cases. If the PTC sensor is loose or not on the
  element, repair it. The swap applies after a reboot. The firmware never
  changes the mapping by itself.
- **pass** is stored in NVS. Later boots skip the check. A change of the active
  thermistor mapping or of a calibration offset makes the stored pass invalid at
  the next boot. `first_heat_rerun=1` forgets the stored result.
- NVS: the latch drops the relay first, then the result is written. A failed
  write is retried every 2 s. The write sets the stored state to "unknown" first.
  It writes the new state last. Thus a failure part way never keeps an old pass
  (fail-closed: the check runs again). A re-run during the tick of a result
  wins over that result.

Supported range (the host tests assert that such a unit never latches):
- Element power ≥ 150 W while the relay is on (300 W nominal). A 230 V unit on
  120 V mains (about 75 W) is **out of spec** and can latch "no-rise".
- The element can reach ≥ 30 K above the air.
- Both thermistors agree within ±2 °C at the same temperature.

Recovery from a false trip: build with `CONFIG_PB_FIRST_HEAT_REPORT_ONLY=y`
(Kconfig, default `n`). The check then reports only. The option is inverted on
purpose: a build that does not know the option enforces the check. It never latches and never
stores a result. There is no runtime toggle. A safe dev-board build
(`CONFIG_PB_DEVBOARD_SAFE`, no mains, no element) also reports only.

Limits of the check:
- It can only **remove** power. It writes the relay only through the existing
  trip path, never changes the thermistor mapping, and never changes a cutoff,
  foldback point, ceiling or watchdog value.
- The thresholds are **provisional**. They come from the stock firmware (2–3 °C
  in 12 s). They are not yet checked against a first-heat `/diag` capture of an
  82 kΩ and a 33 kΩ board.
- With crossed leads and outlet air that follows the element closely, the
  result can be "uncertain" instead of "wrong roles". The fixed 85 °C cutoff,
  which then watches the element, still trips.
- The stock firmware's `ntc_bit` key is read at boot and shown as a hint only.

## Front-panel panic-off (long-press)
Holding **any** front-panel button for 2 s latches the heater off and drops the
mode to OFF. It is called **panic-off**, deliberately, and is **not** a
safety-rated emergency stop — it is software running on the same MCU as the rest
of the control loop, so a firmware fault could defeat it. The Layer-1 bonded
cutoff and Layer-2 PTC physics remain the actual emergency layer.
The panic-off latch is RAM-only and clears on reboot (it is a manual stop, not a
hazard trip); reboot still starts with the SSR, mode, and target OFF, and never
restores an active heat command. Hazard-driven trips, by contrast, **persist** —
see below.

How it stays inside the safety model:
- The button task never writes the SSR GPIO. `pb_policy_request_panic_off()`
  sets the same mux-guarded latch as an over-temp trip
  (`pb_heater_request_panic_off()`, no GPIO write) and then wakes the control
  task, which drops the SSR from its own context — preserving the
  single-SSR-writer invariant.
- The wake means the drop happens on the very next control-task scheduling, not
  at the next 500 ms periodic tick. Host tests verify that the wake is issued
  before diagnostic logging, and the physical Panda bench verified the complete
  panic-off transition. Electrical long-event-to-SSR-low latency has not been
  instrumented, so no sub-20-ms measurement is claimed; that trace is optional
  characterization, not a safety or release gate.
- The transition is attributed to the button (not the generic safety path), so
  it invalidates any remote control lease immediately; a reconnecting
  dashboard/Klipper cannot silently restore heat.

A **long-press on Power while a fault is latched** attempts a fault clear instead
of a redundant panic. The clear only succeeds if the underlying condition has
recovered — the next control tick re-evaluates safety and re-latches if the trip
still holds, so an accidental press cannot revive an unsafe device.

## Persistent fault latch (survives a power cycle)
A **hazard-driven** safety trip — PTC/chamber over-temp, a sensor fault while
heating, or the comms-loss watchdog — is written to NVS (a failed write is
retried) and **restored at boot**. A RAM-only latch (panic-off, the permanent
inhibit, or an unreadable-store boot latch) does not stop this: the hazard cause
goes to NVS. A hazard cause also replaces a persisted generic `safety trip` cause
(for example from a controller-lease watchdog) once. A device that tripped and
then lost power comes back up **heater-OFF with heat/mode commands inhibited**,
rather than silently ready to heat, and stays that way until an **explicit** clear after the
condition has recovered (the next tick re-latches if it hasn't). The persisted
cause is a small, range-checked numeric code; a live reason string carries
session detail but is not persisted.
A first-heat "wrong roles" or "no-rise" latch is written to NVS and restored in the same way.

Boot restore is **fail-safe**: if the persisted state cannot be read reliably it
comes up latched (`persisted fault state unreadable`). A fresh device (no NVS
namespace yet) is not a fault. Clearing is **persist-first** — the NVS latch is
cleared *before* the RAM latch, so a failed persist keeps the heater latched and
the API returns HTTP 500 rather than falsely reporting the fault gone. NVS writes
each key to flash at once, so the clear writes the latch key last: a failure part
way leaves flash latched (at worst with a generic cause, which the next control
tick writes again).

This is deliberately separate from heat resume: the persisted latch is the **only**
fault state that survives a reboot. The active mode, target, deadline, and lease
are never persisted — the device always boots OFF. The permanent inhibit
(`pb_heater_inhibit`) is likewise **not** persisted, so a power cycle lifts it (a
transient init failure must not brick the device across reboots).

## Residual-heat purge (session-gated, hysteresis)
After heat has run **this session**, the cooldown fan keeps running while the
chamber **or** PTC sensor is hot. The release temperature is the user-configurable
`cool_release` (30–65 °C, default 40 °C); the fan engages one 3 °C hysteresis band
above it and releases only once **both** sensors are below `cool_release` (an
unknown/faulted sensor keeps the fan on). It is strictly session-gated: the fan
**never** starts on temperature alone, and because that gate is RAM-only, a
**power-cycle-while-hot does not spin the fan**. A fault forces airflow ON
unconditionally, independent of this purge.

## Fan-only filtration is heater-independent and idle-only to enable
The manual filtration blower (`filter` command) and the AUTO fan-only filtration
band drive **only the blower** — they never engage the heater/SSR. Enabling manual
filtration is **idle-only**: turning it on while the heater is heating or the
cooldown purge is running is rejected (`heater_busy`, HTTP 409) so a status-page
toggle can't disturb an active heat cycle; turning it *off* is always allowed. The
AUTO band fails to no-airflow if disabled or if Moonraker disconnects.

## What is NOT protected
- **Over-current** is only the 6.3 A mains fuse. There is no PCB over-temp cutoff.
- Residual risk is chamber over-temp *up to the Layer-1 trip point* — identical
  to the stock firmware's failure ceiling.
- **Panic-off is not an emergency stop** (see above): a firmware fault could
  defeat it. Do not rely on it as your only means of making the device safe.

## Rule for contributors
Any code path that can energize `PB_GPIO_RELAY` must be reachable only after the
`pb_heater` safety checks. Never drive the SSR GPIO directly from another module.
Cross-task "off" requests (buttons, HTTP, watchdogs) must go through a
mux-guarded latch that converges on the next `pb_heater_tick()` — never call
`pb_heater_emergency_off()` (which writes the GPIO) from a task other than the
control task.
