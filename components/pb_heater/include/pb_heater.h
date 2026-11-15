// SPDX-License-Identifier: MIT
// pb_heater — chamber heater control with defense-in-depth over-temp safety.
//
// Hardware backstops that exist REGARDLESS of this firmware (see docs/SAFETY.md):
//   1. Bonded thermal cutoff in the PTC's mains lead (opens on element over-temp,
//      upstream of the SSR — covers a welded-shut SSR).
//   2. PTC ceramic self-limiting (Curie point) — cannot thermally run away.
// This component is the third (soft) layer: correct set-point control + an
// element/chamber over-temp cutoff + a comms-loss watchdog.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"                 // CONFIG_PB_FIRST_HEAT_REPORT_ONLY, CONFIG_PB_DEVBOARD_SAFE
#endif

// Machine-readable fault cause. Persisted across reboot as a u8 (see
// pb_heater_load_fault); a live free-form reason string (pb_heater_fault_reason)
// carries session detail that is NOT persisted. Append new causes at the END so
// the persisted numeric codes stay stable across firmware versions.
typedef enum {
    PB_FAULT_NONE = 0,
    PB_FAULT_PTC_OVERTEMP,        // element over-temp cutoff
    PB_FAULT_CHAMBER_OVERTEMP,    // chamber over-temp cutoff
    PB_FAULT_CHAMBER_SENSOR,      // chamber thermistor open/short/uninit while armed
    PB_FAULT_PTC_SENSOR,          // PTC thermistor open/short/uninit while armed
    PB_FAULT_LINK_LOST,           // comms-loss watchdog while heating
    PB_FAULT_PANIC_OFF,           // front-panel long-press panic-off
    PB_FAULT_INHIBITED,           // permanent inhibit (reboot-only)
    PB_FAULT_EMERGENCY,           // generic external emergency_off trip
    PB_FAULT_NVS_UNREADABLE,      // boot fail-safe: persisted fault state unreadable
    PB_FAULT_HEATER_NO_RISE,      // first-heat check: the PTC did not rise with the relay on
    PB_FAULT_SENSOR_ROLES,     // first-heat check: the chamber rose more than the PTC
    PB_FAULT__COUNT              // sentinel: any stored code >= this is treated as
                                  // corrupt and mapped to a generic latched fault
} pb_fault_reason_t;

// Read-only control telemetry for diagnostics and hardware validation. These
// values describe DragonBreath's product-owned control path; dc_pid remains a
// board-neutral math primitive with no knowledge of approach limits, safety
// governors, or the physical SSR.
typedef enum {
    PB_HEATER_CONSTRAINT_OFF = 0,
    PB_HEATER_CONSTRAINT_NONE,
    PB_HEATER_CONSTRAINT_APPROACH_LIMIT,
    PB_HEATER_CONSTRAINT_TARGET_REACHED,
    PB_HEATER_CONSTRAINT_LOCAL_FOLDBACK,
    PB_HEATER_CONSTRAINT_ELEMENT_FOLDBACK,
    PB_HEATER_CONSTRAINT_PID_ERROR,
} pb_heater_constraint_t;

typedef struct {
    // PID output after DragonBreath's active approach limit (normalized 0..1).
    float commanded_duty;
    // Product-owned maximum duty for the current temperature error (0..1).
    float approach_limit;
    pb_heater_constraint_t constraint;
} pb_heater_telemetry_t;

// Chamber-control method. BANGBANG is the v1.1.15 hysteresis on/off drive (the
// default); PID is the v1.1.16 dc_pid path with the near-target approach cap.
// Both keep every safety layer (over-temp/sensor/comms trips, local + element
// foldback); only the duty decision differs. Selectable so both can be tested.
typedef enum {
    PB_HEATER_METHOD_BANGBANG = 0,   // v1.1.15 — default
    PB_HEATER_METHOD_PID      = 1,   // v1.1.16 — opt-in
} pb_heater_method_t;

// Pure fail-safe decision for the boot-time fault restore (pb_heater_load_fault),
// inline so it can be host-tested without an NVS backend. Given the outcome of
// reading the persisted latch/code, decides whether to come up latched and with
// which code:
//   - namespace/key never written (fresh device)  -> NOT latched.
//   - a genuine read error (open or latch read)    -> FAIL SAFE: latch, NVS_UNREADABLE.
//   - a readable latch flag == 1                   -> latched with the stored code;
//       an out-of-range/NONE stored code is corrupt -> latch anyway (PB_FAULT_EMERGENCY).
// open_ok / latch_read_ok are false ONLY on a real error (the not-found cases are
// signalled separately via ns_not_found / latch_not_found).
static inline bool pb_heater_fault_decide(bool open_ok, bool ns_not_found,
                                          bool latch_read_ok, bool latch_not_found,
                                          uint8_t latch_val, uint8_t code_val,
                                          pb_fault_reason_t *out_code)
{
    pb_fault_reason_t code = PB_FAULT_NONE;
    bool latched = false;
    if (ns_not_found || latch_not_found) {
        latched = false;                                   // fresh device / never latched
    } else if (!open_ok || !latch_read_ok) {
        latched = true; code = PB_FAULT_NVS_UNREADABLE;    // can't read -> fail safe
    } else if (latch_val) {
        latched = true;
        code = (code_val < PB_FAULT__COUNT && code_val != PB_FAULT_NONE)
               ? (pb_fault_reason_t)code_val : PB_FAULT_EMERGENCY;   // corrupt code -> generic
    }
    if (out_code) *out_code = code;
    return latched;
}

// --- Safety limits ----------------------------------------------------------
// Fixed hardware-safety cutoffs — NEVER user-configurable.
#define PB_HEATER_PTC_CUTOFF_C   105.0f   // element over-temp -> force off (stock parity)
#define PB_HEATER_CHAMBER_MAX_C  85.0f    // chamber over-temp -> force off
#define PB_HEATER_HYSTERESIS_C   1.0f

// When regulation uses a remote/printer chamber sensor, the local chamber NTC can
// be much hotter because it sits in DragonBreath's outlet/near-heater airflow. Keep
// that local region below a non-latching SOFT ceiling instead of relying on the 85 C
// emergency trip as normal control. This limiter applies only while an external
// control temperature is active; local-sensor regulation is unchanged.
#define PB_HEATER_LOCAL_FOLDBACK_CUT_C     72.0f
#define PB_HEATER_LOCAL_FOLDBACK_RESUME_C  67.0f

// Pure hysteresis helper for the local thermal limiter. A bad local read holds the
// previous state; the fail-closed sensor-fault path in pb_heater_eval_trip() remains
// authoritative while armed.
static inline bool pb_heater_local_foldback_cut(bool chamber_ok, float chamber_c,
                                                 bool prev_cut)
{
    if (!chamber_ok) return prev_cut;
    if (chamber_c >= PB_HEATER_LOCAL_FOLDBACK_CUT_C) return true;
    if (chamber_c < PB_HEATER_LOCAL_FOLDBACK_RESUME_C) return false;
    return prev_cut;
}
// Soft element-temperature FOLDBACK, a layer strictly BELOW the hard cutoff: it holds
// the PTC element just under the 105 C trip so a marginal/hot install can reach set-point
// instead of tripping and needing a manual clear. It never adds heat and never delays the
// hard cutoff — if the element still reaches PB_HEATER_PTC_CUTOFF_C (welded SSR, runaway,
// sensor issue) the latching trip fires exactly as before.
//
// Implemented as a HYSTERESIS hard-cut rather than a proportional duty ramp: a linear
// ramp (full power at 100 C -> 0 only at 105 C) proved too gentle on a hot-running board
// — ~50% duty at 102-103 C kept feeding the element so it ratcheted up to ~104.8 C (just
// shy of the 105 C watchdog) with no real cool-down between the fast on/off ticks. The
// hysteresis instead forces the SSR FULLY OFF at the cut point and keeps it off until the
// element cools below the resume point, giving a genuine cool-down cycle and capping the
// peak with margin. The thresholds are PER BOARD VARIANT (selected by the Rref strap, see
// pb_ntc_rref_kohm()): the 33 kOhm (V1.0) board runs the element hotter / ratchets, so it
// gets a lower, more conservative cut; the 82 kOhm (V1.0.1) board tops out cooler and can
// hold a higher cut without approaching 105. Both stay below the 105 C hard cutoff.
#define PB_HEATER_PTC_FOLDBACK_CUT_82K_C     102.0f   // 82k board: element >= this -> cut
#define PB_HEATER_PTC_FOLDBACK_RESUME_82K_C   99.0f   //           hold off until below this
#define PB_HEATER_PTC_FOLDBACK_CUT_33K_C      99.0f   // 33k board: element >= this -> cut
#define PB_HEATER_PTC_FOLDBACK_RESUME_33K_C   96.0f   //           hold off until below this

// Resolve the foldback cut/resume thresholds for a board's Rref (kOhm). The 33 kOhm
// variant (incl. a floating strap defaulted to 33k) gets the conservative pair; anything
// else uses the 82 kOhm pair (the higher, more permissive default).
static inline void pb_heater_foldback_thresholds(int rref_kohm, float *cut_c, float *resume_c)
{
    if (rref_kohm == 33) { *cut_c = PB_HEATER_PTC_FOLDBACK_CUT_33K_C; *resume_c = PB_HEATER_PTC_FOLDBACK_RESUME_33K_C; }
    else                 { *cut_c = PB_HEATER_PTC_FOLDBACK_CUT_82K_C; *resume_c = PB_HEATER_PTC_FOLDBACK_RESUME_82K_C; }
}

// Optional user override of the foldback CUT (advanced setting; see the settable API
// below). The resume point always trails the cut by one hysteresis band. The override
// is bounded well under the fixed 105 C hard cutoff, so it can never defeat over-temp
// protection — it only shifts where the SOFT foldback engages. 0 = "auto" (use the
// board's per-Rref default from pb_heater_foldback_thresholds).
#define PB_HEATER_PTC_FOLDBACK_BAND_C   3.0f    // resume = cut - band
#define PB_HEATER_FB_CUT_MIN_C          90.0f
#define PB_HEATER_FB_CUT_MAX_C         104.0f   // < 105 hard cutoff, always

// Resolve the EFFECTIVE cut/resume: a positive user override wins (resume = cut - band),
// otherwise fall back to the board's per-Rref default. Pure/inline for host testing.
static inline void pb_heater_effective_foldback(float user_cut_c, int rref_kohm,
                                                 float *cut_c, float *resume_c)
{
    if (user_cut_c > 0.0f) { *cut_c = user_cut_c; *resume_c = user_cut_c - PB_HEATER_PTC_FOLDBACK_BAND_C; }
    else                     pb_heater_foldback_thresholds(rref_kohm, cut_c, resume_c);
}

// Pure element-foldback hysteresis, inline so it is host-testable without hardware.
// Returns whether to FORCE the SSR off this tick given the element temperature, whether
// its reading is valid, the previous cut-latch state, and the board's cut/resume points:
//   ptc >= cut_c               -> cut  (true)
//   ptc <  resume_c            -> allow (false)
//   resume_c <= ptc < cut_c    -> hold the previous state (the hysteresis band)
//   reading not OK             -> hold the previous state (a bad read is owned by the
//                                 sensor-fault trip / hard cutoff, not by the foldback)
static inline bool pb_heater_foldback_cut(bool ptc_ok, float ptc_c, bool prev_cut,
                                          float cut_c, float resume_c)
{
    if (!ptc_ok)            return prev_cut;
    if (ptc_c >= cut_c)     return true;
    if (ptc_c <  resume_c)  return false;
    return prev_cut;
}
// Settable set-point ceiling: default + absolute cap + floor. Raising the
// production cap above 70 C is a separate hw-validation item (80 C leaves only
// 5 C below the fixed 85 C chamber trip — not enough margin without measured
// worst-case lag/overshoot), so ABS_MAX stays at 70.
#define PB_HEATER_MAX_TARGET_C_DEFAULT  70.0f
#define PB_HEATER_ABS_MAX_TARGET_C      70.0f
#define PB_HEATER_MIN_TARGET_C          30.0f
// Comms deadman: no controller for this long while heating -> latch off. Runtime-
// configurable within [MIN, MAX]; never disabled or extended beyond MAX (5 min).
#define PB_HEATER_COMMS_TIMEOUT_MS_DEFAULT  (5 * 60 * 1000)
#define PB_HEATER_COMMS_TIMEOUT_MS_MIN      (10 * 1000)
#define PB_HEATER_COMMS_TIMEOUT_MS_MAX      (5 * 60 * 1000)
// Residual-heat cooldown-purge "cool down to" temperature: after heat ran, the fan
// keeps running until the chamber + PTC drop below this, then stops. Raise it for a
// hot room where the sensors can't reach the default (else the fan runs forever).
// This is a comfort/wear setting, NOT a safety cutoff — fault airflow and the fixed
// over-temp trips are independent.
#define PB_HEATER_COOL_RELEASE_C_DEFAULT    40.0f
#define PB_HEATER_COOL_RELEASE_MIN_C        30.0f
#define PB_HEATER_COOL_RELEASE_MAX_C        65.0f

// Pure safety-trip decision for pb_heater_tick(), inline so the fail-closed
// priority ordering can be host-tested without the ADC/SSR/RTOS backend. Given the
// freshest per-channel sensor reads (status-OK flags + instantaneous °C), whether a
// target is armed, and whether the comms deadman has expired, returns the fault the
// tick must latch — or PB_FAULT_NONE if it is safe to run the control loop. The
// order is load-bearing and MUST stay identical to pb_heater_tick():
//   1. PTC over-temp     — only trusted when the PTC sensor reads valid
//   2. chamber over-temp — only trusted when the chamber sensor reads valid
//   3. armed + chamber sensor NOT ok -> fail-closed (blind heater)
//   4. armed + PTC sensor NOT ok     -> fail-closed (unmonitored element)
//   5. armed + comms deadman expired -> link-lost watchdog
// The over-temp cutoffs fire regardless of `armed`; the sensor-fault and comms
// trips are gated on `armed` (an idle heater with a disconnected sensor is not a
// hazard). A non-OK sensor also means its temperature is NAN — the `*_ok` gate on
// each over-temp check keeps a NAN comparison from ever masking a real trip.
static inline pb_fault_reason_t pb_heater_eval_trip(
    bool ptc_ok, float ptc_c, bool chamber_ok, float chamber_c,
    bool armed, bool link_expired)
{
    if (ptc_ok && ptc_c >= PB_HEATER_PTC_CUTOFF_C)          return PB_FAULT_PTC_OVERTEMP;
    if (chamber_ok && chamber_c >= PB_HEATER_CHAMBER_MAX_C) return PB_FAULT_CHAMBER_OVERTEMP;
    if (armed && !chamber_ok)                               return PB_FAULT_CHAMBER_SENSOR;
    if (armed && !ptc_ok)                                   return PB_FAULT_PTC_SENSOR;
    if (armed && link_expired)                              return PB_FAULT_LINK_LOST;
    return PB_FAULT_NONE;
}

// --- First-heat check ---------------------------------------------------------
// A plausibility check of the thermistor roles and the element, like the stock
// firmware's first-heat check, but it never swaps the channels.
//
// Window: a relay OFF->ON edge opens a window, unless the element is already
// warm (see the start bands below). The start reading of each channel is the
// median of its first three readings (one glitch cannot make a false start).
// The window counts relay ON time only. A rise is the peak minus the start, in
// integer tenths of a degree. The peak uses the lower of each two consecutive
// readings (one glitch cannot make a false rise). With p = PTC rise and
// c = chamber rise:
//   from PB_FH_MIN_ON_MS of ON time:
//     p >= PB_FH_PASS_RISE_DC and p > 2 c                               -> PASS
//   from PB_FH_MIN_ON_MS of ON time, only within PB_FH_FAIL_WALL_MS of the open:
//     c >= 2 p + PB_FH_ROLES_MARGIN_DC and c >= PB_FH_ROLES_EARLY_DC    -> WRONG_ROLES
//   at PB_FH_MAX_ON_MS of ON time, only within PB_FH_FAIL_WALL_MS of the open:
//     c >= 2 p + PB_FH_ROLES_MARGIN_DC                                  -> WRONG_ROLES
//     p < PB_FH_NO_RISE_DC                                              -> NO_RISE
//     other cases (both rose by similar amounts)                       -> UNCERTAIN
// A failed result thus needs mostly continuous heat (duty >= 50 %): short ON
// pulses at a low duty do not add up to a rise. A low-duty window can still
// pass. Without a result after PB_FH_MAX_WALL_MS the window closes (a restart,
// counted in `restarts`) and the next relay OFF->ON edge opens a new one.
// WRONG_ROLES and NO_RISE latch the heater off with their own fault cause.
// UNCERTAIN only reports (no latch, nothing stored). The next heat checks again.
//
// Why WRONG_ROLES is "chamber rise >= 2 x PTC rise + 2 C": the chamber
// thermistor sits in the heater's own outlet air. At full power the outlet air
// can rise 75-150 K above the inlet (estimate, 0.1-0.2 m3/min airflow), so with
// crossed leads the "PTC" channel (the outlet air) also rises quickly: an
// absolute PTC limit would miss crossed leads. But the outlet air cannot get
// hotter than the element, and it heats later: in a correct unit the PTC rise is
// larger than the chamber rise. A chamber rise of twice the PTC rise plus a
// margin thus means crossed leads OR a PTC thermistor that lags the element or
// is not on it. Both are faults: the PTC cutoff cannot see the element. A
// well-coupled PTC thermistor rises about 1.5-3 C/s (#122 trace: 1.44 C/s), so
// a correct unit passes long before 30 s. When both rises are similar, the
// result is UNCERTAIN, never WRONG_ROLES.
//
// Supported plant (the host-test oracle asserts that such a plant never
// latches): element power >= 150 W while the relay is on (300 W nominal: a
// 230 V unit on 120 V mains, about 75 W, is out of spec), the element can reach
// >= 30 K above the air, and both thermistors agree within +-2 C at the same
// temperature. Out of spec, a correct unit can latch NO_RISE.
// CONFIG_PB_FIRST_HEAT_REPORT_ONLY=y is the build-time recovery: report only.
//
// Start bands: a PTC start reading >= PB_FH_HOT_START_DC, or >= the chamber
// reading + PB_FH_WARM_START_DC, means the element is already warm (it can be
// at its equilibrium and cannot rise). Then no window opens until the next
// relay OFF->ON edge.
//
// The check can only remove heat: it latches through the same trip path as the
// hazard checks, never changes the thermistor mapping, and never changes a
// limit. A stored PASS skips the check on later boots until the thermistor
// mapping or an offset changes.
//
// PROVISIONAL thresholds: they come from the stock firmware (2-3 C in 12 s) and
// are not yet checked against a first-heat /diag capture of an 82k and a 33k board.
#define PB_FH_MIN_ON_MS        12000   // ON time before a result (stock: 12 s)
#define PB_FH_MAX_ON_MS        30000   // ON time for the final result
#define PB_FH_FAIL_WALL_MS     60000   // a failed result only within this wall time of the open
#define PB_FH_MAX_WALL_MS     300000   // wall time limit of one window; then a restart
#define PB_FH_PASS_RISE_DC        30   // 3.0 C PTC rise for a pass
#define PB_FH_NO_RISE_DC          20   // PTC rise below 2.0 C at 30 s -> NO_RISE (stock: 2 C)
#define PB_FH_ROLES_MARGIN_DC     20   // chamber rise >= 2 x PTC rise + 2.0 C -> WRONG_ROLES
#define PB_FH_ROLES_EARLY_DC     100   // before 30 s only with a chamber rise >= 10.0 C
#define PB_FH_HOT_START_DC       700   // PTC >= 70.0 C at the edge: no window
#define PB_FH_WARM_START_DC      100   // PTC >= chamber + 10.0 C at the edge: no window

// Fail-closed default: a build without the option enforces the check.
#if defined(CONFIG_PB_DEVBOARD_SAFE) || defined(CONFIG_PB_FIRST_HEAT_REPORT_ONLY)
#define PB_FH_ENFORCED 0               // dev board, or the recovery build: report only
#else
#define PB_FH_ENFORCED 1
#endif

typedef enum {
    PB_FH_IDLE = 0,      // no window open (no relay ON yet, or stopped/aborted)
    PB_FH_ARMED,         // window open, ON time below PB_FH_MIN_ON_MS
    PB_FH_EVAL,          // ON time at or above PB_FH_MIN_ON_MS, no decision yet
    PB_FH_PASS,
    PB_FH_WRONG_ROLES,   // crossed leads, or the PTC thermistor is not on the element
    PB_FH_NO_RISE,
    PB_FH_UNCERTAIN,     // both channels rose: report only, the next heat checks again
} pb_fh_state_t;

// The result in NVS. Values are persisted: append only.
typedef enum {
    PB_FH_STORED_UNKNOWN = 0,
    PB_FH_STORED_PASS    = 1,
    PB_FH_STORED_WRONG_ROLES = 2,
    PB_FH_STORED_NO_RISE = 3,
} pb_fh_stored_t;

// The stock firmware's NVS key app_nvs/ntc_bit (a hint only, never applied).
typedef enum {
    PB_FH_STOCK_NONE = 0,    // key not present
    PB_FH_STOCK_UNKNOWN,     // 0: stock did not decide
    PB_FH_STOCK_CROSSED,     // 1: stock found crossed roles (and swapped silently)
    PB_FH_STOCK_NORMAL,      // 2: stock found normal roles
    PB_FH_STOCK_INVALID,     // another value, size or type, or a read error
} pb_fh_stock_hint_t;

typedef struct {
    pb_fh_state_t state;          // this session
    pb_fh_stored_t stored;        // NVS (UNKNOWN if the stored mapping does not match)
    pb_fh_stock_hint_t stock_hint;
    int32_t ptc_rise_dc;          // last measured rises, tenths of a degree
    int32_t ch_rise_dc;
    uint32_t on_ms;               // ON time of the open or last window
    uint32_t restarts;            // windows closed without a result this boot
    uint32_t skipped;             // relay-on edges with a warm element (no window)
    bool enforced;                // false: report only (dev board or REPORT_ONLY)
} pb_heater_first_heat_t;

// Pure decision of the first-heat check, inline for host tests.
typedef enum {
    PB_FH_EVAL_WAIT = 0, PB_FH_EVAL_PASS, PB_FH_EVAL_WRONG_ROLES, PB_FH_EVAL_NO_RISE,
    PB_FH_EVAL_UNCERTAIN,
} pb_fh_eval_t;
static inline pb_fh_eval_t pb_heater_first_heat_eval(uint32_t on_ms, uint32_t wall_ms,
                                                     int32_t ptc_rise_dc, int32_t ch_rise_dc)
{
    if (on_ms < PB_FH_MIN_ON_MS) return PB_FH_EVAL_WAIT;
    if (ptc_rise_dc >= PB_FH_PASS_RISE_DC && ptc_rise_dc > 2 * ch_rise_dc) return PB_FH_EVAL_PASS;
    if (wall_ms > PB_FH_FAIL_WALL_MS) return PB_FH_EVAL_WAIT;   // low duty: no failed result
    bool roles = ch_rise_dc >= 2 * ptc_rise_dc + PB_FH_ROLES_MARGIN_DC;
    if (roles && ch_rise_dc >= PB_FH_ROLES_EARLY_DC) return PB_FH_EVAL_WRONG_ROLES;
    if (on_ms < PB_FH_MAX_ON_MS) return PB_FH_EVAL_WAIT;
    if (roles) return PB_FH_EVAL_WRONG_ROLES;
    if (ptc_rise_dc < PB_FH_NO_RISE_DC) return PB_FH_EVAL_NO_RISE;
    return PB_FH_EVAL_UNCERTAIN;
}

// True if the start readings (tenths) show an element that is already warm.
static inline bool pb_heater_first_heat_warm(int32_t ptc_dc, int32_t ch_dc)
{
    return ptc_dc >= PB_FH_HOT_START_DC || ptc_dc - ch_dc >= PB_FH_WARM_START_DC;
}

// Median of three (the start reading of a window).
static inline int32_t pb_heater_median3(int32_t a, int32_t b, int32_t c)
{
    if (a > b) { int32_t t = a; a = b; b = t; }
    if (b > c) b = c;
    return a > b ? a : b;
}

// Temperature in C to integer tenths, rounded half away from zero and clamped
// to [-100, 300] C. The one float boundary of the check.
static inline int32_t pb_heater_c_to_dc(float c)
{
    if (!(c > -100.0f)) c = -100.0f;      // also NAN
    if (c > 300.0f) c = 300.0f;
    return (int32_t)(c * 10.0f + (c >= 0.0f ? 0.5f : -0.5f));
}

// Read the stored result and the stock hint. Call AFTER pb_ntc_load_calibration()
// (the stored result is valid only for the same mapping and offsets) and before
// the control task starts. A read error gives UNKNOWN: the check runs again.
void pb_heater_load_first_heat(void);
// Copy a snapshot (any task).
void pb_heater_get_first_heat(pb_heater_first_heat_t *out);
// Forget the stored result and run the check again on the next heat (any task).
// Returns the NVS error if the reset did not persist. The RAM reset applies in
// all cases, and the control task retries the write.
esp_err_t pb_heater_first_heat_rerun(void);
const char *pb_heater_first_heat_state_str(pb_fh_state_t s);
const char *pb_heater_first_heat_stored_str(pb_fh_stored_t s);
const char *pb_heater_first_heat_stock_str(pb_fh_stock_hint_t h);

// Bring up the SSR GPIO in a guaranteed-OFF state. Call before anything can
// request heat. Idempotent.
esp_err_t pb_heater_init(void);

// Set the desired chamber temperature (clamped to the runtime ceiling returned
// by pb_heater_get_max_target_c(), never above PB_HEATER_ABS_MAX_TARGET_C).
// 0 (or below) disables heating. Returns:
//   ESP_OK             - accepted
//   ESP_ERR_INVALID_ARG   - non-finite (NaN/Inf) target, rejected
//   ESP_ERR_INVALID_STATE - a positive target while a safety fault is latched
//                           (clear it with pb_heater_clear_fault first); maps to
//                           HTTP 409. Heat is never queued behind a fault latch.
esp_err_t pb_heater_set_target_c(float target_c);
float pb_heater_get_target_c(void);

// Copy one lock-consistent diagnostic snapshot. This is observational only and
// has no effect on PID state, watchdogs, leases, or heater output.
void pb_heater_get_telemetry(pb_heater_telemetry_t *out);
const char *pb_heater_constraint_str(pb_heater_constraint_t constraint);

// Optional external chamber measurement used ONLY for set-point regulation.
// Pass NAN to fall back to the local chamber NTC. The local chamber NTC and PTC
// remain authoritative for over-temperature trips, sensor-fault detection, and
// all other safety decisions regardless of this value.
void pb_heater_set_control_chamber_c(float temp_c);

// --- Runtime-configurable, persisted settings (pb_heater is the sole owner) ---
// Load persisted settings from NVS (namespace app_nvs). MUST be called AFTER
// nvs_init() — pb_heater_init() only sets conservative defaults (nvs isn't up
// yet). Values are clamped on read.
void  pb_heater_load_config(void);
// Restore a persisted fault latch from NVS. MUST be called AFTER nvs_init() and
// BEFORE the control task / HTTP API are exposed, so a device that latched off
// before a power cycle comes back up heater-OFF and commands-inhibited rather than
// silently ready to heat. Fail-safe: if the persisted state cannot be read
// reliably it latches a generic fault (PB_FAULT_NVS_UNREADABLE). A namespace that
// was never written (fresh device) is NOT a fault.
void  pb_heater_load_fault(void);
// Settable set-point ceiling. Setter clamps to [MIN_TARGET_C, ABS_MAX_TARGET_C],
// persists it, and pulls a live target down if it now exceeds the cap.
esp_err_t pb_heater_set_max_target_c(float max_c);
float     pb_heater_get_max_target_c(void);
// Comms deadman timeout. Setter clamps to [MS_MIN, MS_MAX] (never disabled or
// extended past 5 min) and persists it.
esp_err_t pb_heater_set_comms_timeout_ms(uint32_t ms);
uint32_t  pb_heater_get_comms_timeout_ms(void);
// Cooldown-purge "cool down to" temperature. Setter clamps to
// [COOL_RELEASE_MIN_C, COOL_RELEASE_MAX_C] and persists it. Read by pb_policy's
// residual-heat purge (pb_purge_decide).
esp_err_t pb_heater_set_cool_release_c(float c);
float     pb_heater_get_cool_release_c(void);
// Advanced: user override of the element-foldback CUT temperature. Setter clamps a
// positive value to [FB_CUT_MIN_C, FB_CUT_MAX_C] and persists it; pass <= 0 to clear
// the override and return to the board's per-Rref default. Getter returns the raw
// stored value (0 = auto). The resume point always trails by one hysteresis band.
esp_err_t pb_heater_set_fb_cut_c(float c);
float     pb_heater_get_fb_cut_c(void);

// Select the chamber-control method (persisted). Default is BANGBANG (v1.1.15);
// PID (v1.1.16) is opt-in. A change takes effect on the next control tick and
// resets the newly selected path's history so it starts clean.
esp_err_t          pb_heater_set_method(pb_heater_method_t method);
pb_heater_method_t pb_heater_get_method(void);

// Feed the comms watchdog: call whenever a live controller link is confirmed
// (Moonraker connected, or a fresh command). If not called within
// pb_heater_get_comms_timeout_ms() while heating, the heater latches off.
void pb_heater_notify_link_alive(void);

// Periodic control tick (call at ~2 Hz). Reads the local chamber + PTC temps,
// enforces all safety cutoffs from those local sensors, and drives the SSR from
// the shared dc_pid controller using the optional external regulation temperature
// when one is supplied (otherwise the local chamber NTC is the process variable).
void pb_heater_tick(void);

// Immediate, latching shutoff. Clears the target and latches off. Heat stays off
// until pb_heater_clear_fault() is called AND the condition has cleared.
// CONTROL-TASK ONLY: it writes the SSR GPIO directly, so calling it from another
// task would break the single-SSR-writer invariant. Off-task callers use
// pb_heater_request_panic_off() instead.
void pb_heater_emergency_off(const char *reason);

// Latch the heater off from ANY task without touching the SSR GPIO. Sets the
// same latch/target/reason as emergency_off but leaves the physical drop to the
// next pb_heater_tick() (control task), so the single-writer invariant holds.
// Used by the front-panel long-press ("panic-off"). To minimize the drop
// latency, wake the control task immediately after calling this.
void pb_heater_request_panic_off(const char *reason);

// Explicitly clear a latched safety fault (over-temp / sensor fault / comms loss).
// Setting a new target does NOT auto-clear it — this is a deliberate reset. The
// next tick re-evaluates safety and re-latches if the fault still holds. Also
// clears the persisted latch in NVS. Returns:
//   ESP_OK                - cleared (and the NVS clear committed)
//   ESP_ERR_INVALID_STATE - a permanent inhibit is latched (not clearable)
//   other                 - the NVS clear failed; the fault stays latched (RAM and
//                           flash). Callers should surface this (HTTP 500) rather
//                           than report success.
esp_err_t pb_heater_clear_fault(void);

// PERMANENT inhibit: force the heater off and refuse all heat until reboot. Unlike
// a latched fault this is NOT clearable by pb_heater_clear_fault()/API clear_fault —
// only a power cycle lifts it. Use for conditions under which the heater must
// never run again this boot (e.g. the control-loop watchdog could not be armed,
// so a hung loop would go undetected). Reports as a fault in API v2 state.
void pb_heater_inhibit(const char *reason);

// True once pb_heater_inhibit() has been called (reboot-only).
bool pb_heater_is_inhibited(void);

// True if a safety trip has latched the heater off (needs an explicit clear).
bool pb_heater_is_faulted(void);

// The reason string from the most recent trip (NULL if none / after clear). This
// is live session detail and is NOT persisted; after a reboot-restored latch it
// reflects the canonical string for the persisted code.
const char *pb_heater_fault_reason(void);

// The machine-readable cause of the current latch (PB_FAULT_NONE if not faulted).
// Stable across firmware versions and survives a reboot, unlike the reason string.
pb_fault_reason_t pb_heater_fault_code(void);

// Canonical, stable string for a fault code (never NULL; out-of-range -> generic).
const char *pb_heater_fault_str(pb_fault_reason_t code);

// True if the SSR is currently commanded on (momentary time-proportion state).
bool pb_heater_is_on(void);

// True in "heat mode": a target is armed and no fault is latched. Steady across
// the SSR's time-proportion cycling — use this (not pb_heater_is_on) for the fan/LED.
bool pb_heater_heat_mode(void);
