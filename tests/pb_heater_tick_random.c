// SPDX-License-Identifier: MIT
// Randomized invariant test for the REAL pb_heater_tick(). Fixed seeds, so a
// failure is reproducible: the test prints the seed, and PB_HEATER_SEED=N runs
// only that seed. PB_HEATER_SEEDS sets the number of default seeds and
// PB_HEATER_STEPS the number of random actions for each seed.
//
// Each step does one random action (target, link, time, sensors, panic-off,
// clear, settings, NVS failures, reboot ...) and then one or more control ticks.
// After each tick an independent oracle checks:
//   - the relay is on only if no trip condition holds, no fault is latched, the
//     target is positive, both sensors read OK, the link is fresh, the element
//     foldback and the local foldback do not cut, and the regulation
//     temperature is below the target (pb_heater requires a fresh link in all
//     modes; pb_policy refreshes it for the local modes),
//   - a trip condition turns the relay off in the same tick, latches with the
//     correct code and (for a new latch) persists to NVS when NVS works,
//   - the target never exceeds the ceiling, and the ceiling never exceeds 70 C,
//   - bang-bang: the relay is on when all of the above permit heat and the
//     chamber is below the hysteresis band (so the test is not vacuous),
//   - PID: the relay follows the reported duty at the window extremes, and
//     the reported duty is 0 while a foldback cuts.
//   - first-heat check: an independent model of the window (relay ON time,
//     start readings, glitch-filtered peaks, decision) predicts the session
//     state, the PASS/CROSSED/NO_RISE result, its fault latch and its NVS record
//     in each tick. A hazard trip in the same tick aborts the window instead.
// At the end, the totals over all default seeds must show each trip type, an
// element foldback cut, a local foldback cut, relay-on ticks, liveness checks,
// and each first-heat result.
//
// Where each oracle rule comes from:
//   From docs/SAFETY.md (the specification):
//     - the cutoffs 105 C (PTC) and 85 C (chamber), always active and latching
//     - sensor faults and the comms watchdog trip only while heating
//     - the watchdog timeout is clamped to [10 s, 5 min], the ceiling to 70 C
//     - the foldback cut points per board (82k: 102 C, 33k: 99 C) and the
//       override band [90, 104] C
//     - hazard trips persist to NVS, panic-off and inhibit do not, and the
//       boot latch restore is fail-safe
//     - clear_fault is persist-first and does not restart the heat
//   Copied from the code (pb_heater.h, pb_heater.c, pb_heater_pid.h), because
//   SAFETY.md does not give them:
//     - the trip priority order (PTC, chamber, chamber sensor, PTC sensor, link)
//     - "expired" means age > timeout (not >=)
//     - the foldback resume points (82k: 99 C, 33k: 96 C, override: cut - 3 C)
//     - the local foldback for an external regulation value: cut at 72 C,
//       resume below 67 C
//     - the 1 C bang-bang hysteresis, and the PID window (30 s) and dwell (2 s)
//     - a hazard trip writes NVS only if the current latch is not in NVS yet
//       (so repeated trips while hot do not write NVS again)
//     - the 2 s throttle of the deferred persist retry
//     - the clamps of the other settings (minimum ceiling 30 C, cool_release)
//     - first-heat: the order inside the tick (re-run request, hazard trip,
//       stop, window step, drive, window open after the drive), the min(this,
//       last) glitch filter of the peaks, and the 2 s throttle of the NVS retry
#define _DEFAULT_SOURCE
#include "pb_heater.h"
#include "pb_heater_pid.h"
#include "pb_heater_tick_host_test.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define REBOOT_EXIT_CODE 42

typedef enum { SM_NORMAL, SM_NOISY, SM_STUCK, SM_DROPOUT, SM_JUMP, SM__COUNT } sensor_mode_t;
static const char *const k_mode_name[SM__COUNT] = { "normal", "noisy", "stuck", "dropout", "jump" };

// Plant faults that the first-heat check must find.
typedef enum { PLANT_NORMAL, PLANT_CROSSED, PLANT_DEAD, PLANT_DETACHED, PLANT__COUNT } plant_t;
static const char *const k_plant_name[PLANT__COUNT] = { "normal", "crossed", "dead", "detached_ptc" };

typedef struct {
    uint64_t seed;
    uint64_t rng;
    long step, steps, ticks;
    const char *action;            // string literal, same address in each child

    // Thermal model: an INPUT GENERATOR for plausible temperature traces, not a
    // claim about the physics of the Panda Breath. Two nodes (element, chamber),
    // element power that falls as the element gets hot, chamber loss to ambient
    // that a fan increases. The parameters are random over a wide range.
    double te, tc, ta;
    double pmax_w, ce_j_k, cc_j_k, kec_w_k, kca_w_k, fan_boost, knee_c, knee_w;
    bool fan;

    // Sensor generator, indexed by pb_ntc_channel_t.
    int mode[2];
    float bias[2];                 // like a calibration offset, within +-5 C
    float noise[2];
    float stuck[2];
    int drop_status[2];

    // Simulated controller and inputs.
    bool controller_present;
    float ext_ctrl;                // external regulation temperature or NAN

    // Oracle state (expected values, from SAFETY.md and pb_heater.h).
    bool faulted, inhibited, expect_persist;
    int exp_cause;                 // the cause that NVS must hold (PB_FAULT_NONE: none)
    float exp_target, exp_max, exp_fb_cut;
    uint32_t exp_comms_ms;
    int exp_method;
    int64_t last_link_us;
    bool fb, local;
    bool nvs_write_bad, nvs_read_bad;
    int64_t nvs_healthy_since_us;

    // Statistics.
    long faulted_ticks, armed_ticks;
    long relay_on_ticks, liveness_checks, fb_cut_ticks, local_cut_ticks, reboots;
    long trips[PB_FAULT__COUNT];
    long pid_liveness_checks;
    long panics, clears_ok, clears_failed;

    // First-heat oracle (docs/SAFETY.md, "First-heat check").
    int plant;                     // plant_t
    int fh;                        // expected pb_fh_state_t
    int64_t fh_t0, fh_last, fh_on_us;
    int32_t fh_p0, fh_c0, fh_pprev, fh_cprev, fh_ppeak, fh_cpeak, fh_prise, fh_crise;
    bool fh_rerun;                 // a re-run request that the next tick applies
    int fh_stored;                 // expected pb_fh_stored_t (RAM view)
    bool fh_pending;               // a result write that must land in NVS
    int fh_store_val;
    int32_t fh_store_p, fh_store_c;
    long fh_windows, fh_pass, fh_crossed, fh_norise, fh_uncertain, fh_hazard_aborts, fh_timeouts;
} world_t;

static world_t *W;

// --- random numbers (splitmix64) -------------------------------------------------

static uint64_t rnd64(void)
{
    uint64_t z = (W->rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint32_t rnd_u(uint32_t n) { return (uint32_t)(rnd64() % n); }
static double rnd_f(double lo, double hi) { return lo + (hi - lo) * ((double)(rnd64() >> 11) / 9007199254740992.0); }
static bool chance(double p) { return rnd_f(0.0, 1.0) < p; }

// --- failure report ----------------------------------------------------------------

static const char *status_name(int st)
{
    switch (st) {
    case PB_NTC_OK: return "OK";
    case PB_NTC_OPEN: return "OPEN";
    case PB_NTC_SHORT: return "SHORT";
    case PB_NTC_UNINIT: return "UNINIT";
    default: return "?";
    }
}

static pb_heater_first_heat_t fw_fh(void)
{
    pb_heater_first_heat_t f;
    pb_heater_get_first_heat(&f);
    return f;
}

static void rfail(int line, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
static void rfail(int line, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "\nRANDOM INVARIANT FAILED (line %d): ", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr,
            "\n  seed=%" PRIu64 " step=%ld tick=%ld last_action=%s\n"
            "  rerun: PB_HEATER_SEED=%" PRIu64 " PB_HEATER_STEPS=%ld\n"
            "  now=%.3fs relay=%d is_on=%d faulted=%d (oracle %d) inhibited=%d code=%d\n"
            "  target=%.2f (oracle %.2f) max=%.2f (oracle %.2f) method=%d (oracle %d)\n"
            "  chamber=%s %.2f (%s)  ptc=%s %.2f (%s)  rref=%d  fb_cut=%.2f  ext=%.2f\n"
            "  link_age=%.3fs comms_timeout=%ums  oracle fb=%d local=%d\n"
            "  model te=%.2f tc=%.2f  nvs fault_latch=%d fault_code=%d write_bad=%d read_bad=%d\n"
            "  plant=%s first-heat oracle=%d fw=%d on=%.3fs rise ptc=%d ch=%d (fw %d %d) "
            "stored=%d (fw %d) nvs fh_state=%d\n",
            W->seed, W->step, W->ticks, W->action ? W->action : "-", W->seed, W->steps,
            (double)env->now_us / 1e6, relay(), (int)pb_heater_is_on(),
            (int)pb_heater_is_faulted(), (int)W->faulted, (int)pb_heater_is_inhibited(),
            (int)pb_heater_fault_code(),
            (double)pb_heater_get_target_c(), (double)W->exp_target,
            (double)pb_heater_get_max_target_c(), (double)W->exp_max,
            (int)pb_heater_get_method(), W->exp_method,
            status_name(env->ntc_status[PB_NTC_CHAMBER]), (double)env->ntc_temp[PB_NTC_CHAMBER],
            k_mode_name[W->mode[PB_NTC_CHAMBER]],
            status_name(env->ntc_status[PB_NTC_PTC]), (double)env->ntc_temp[PB_NTC_PTC],
            k_mode_name[W->mode[PB_NTC_PTC]],
            env->rref_kohm, (double)W->exp_fb_cut, (double)W->ext_ctrl,
            (double)(env->now_us - W->last_link_us) / 1e6, (unsigned)W->exp_comms_ms,
            (int)W->fb, (int)W->local, W->te, W->tc,
            nvs_u("fault_latch"), nvs_u("fault_code"), (int)W->nvs_write_bad, (int)W->nvs_read_bad,
            k_plant_name[W->plant], W->fh, (int)fw_fh().state, (double)W->fh_on_us / 1e6,
            (int)W->fh_prise, (int)W->fh_crise, (int)fw_fh().ptc_rise_dc, (int)fw_fh().ch_rise_dc,
            W->fh_stored, (int)fw_fh().stored, nvs_u("fh_state"));
    fflush(stderr);
    exit(1);
}
#define RCHECK(expr, ...) do { if (!(expr)) rfail(__LINE__, __VA_ARGS__); } while (0)

// --- input generators ----------------------------------------------------------------

static void model_advance(double dt_s)
{
    const double h = 0.05;
    bool on = relay() == 1;
    for (double t = 0.0; t < dt_s; t += h) {
        double step = (dt_s - t < h) ? dt_s - t : h;
        double p = on && W->plant != PLANT_DEAD
                   ? W->pmax_w / (1.0 + exp((W->te - W->knee_c) / W->knee_w)) : 0.0;
        double q_ec = W->kec_w_k * (W->te - W->tc);
        double q_ca = W->kca_w_k * (1.0 + (W->fan ? W->fan_boost : 0.0)) * (W->tc - W->ta);
        W->te += step * (p - q_ec) / W->ce_j_k;
        W->tc += step * (q_ec - q_ca) / W->cc_j_k;
    }
}

static void update_sensors(void)
{
    double truth[2];
    truth[PB_NTC_CHAMBER] = W->plant == PLANT_CROSSED ? W->te : W->tc;
    truth[PB_NTC_PTC] = W->plant == PLANT_CROSSED ? W->tc
                      : W->plant == PLANT_DETACHED ? W->ta : W->te;
    for (int ch = 0; ch < 2; ++ch) {
        float v = (float)truth[ch] + W->bias[ch];
        int st = PB_NTC_OK;
        switch (W->mode[ch]) {
        case SM_NOISY:
            v += W->noise[ch] * (float)(rnd_f(-1.0, 1.0) + rnd_f(-1.0, 1.0));
            break;
        case SM_STUCK:
            v = W->stuck[ch];
            break;
        case SM_DROPOUT:
            if (chance(0.5)) st = W->drop_status[ch];
            break;
        case SM_JUMP:
            if (chance(0.05)) v = (float)rnd_f(-60.0, 200.0);
            break;
        default:
            break;
        }
        if (chance(0.5)) v = roundf(v * 10.0f) / 10.0f;   // 0.1 C display steps
        env->ntc_status[ch] = st;
        env->ntc_temp[ch] = v;
    }
}

// --- oracle ------------------------------------------------------------------------------

static bool nvs_writes_work(void) { return !W->nvs_write_bad; }

static pb_fault_reason_t oracle_trip(bool ptc_ok, float ptc, bool ch_ok, float ch,
                                     bool armed, bool expired)
{
    // docs/SAFETY.md, Layer 3, in the order of pb_heater.h.
    if (ptc_ok && ptc >= 105.0f) return PB_FAULT_PTC_OVERTEMP;
    if (ch_ok && ch >= 85.0f)    return PB_FAULT_CHAMBER_OVERTEMP;
    if (armed && !ch_ok)         return PB_FAULT_CHAMBER_SENSOR;
    if (armed && !ptc_ok)        return PB_FAULT_PTC_SENSOR;
    if (armed && expired)        return PB_FAULT_LINK_LOST;
    return PB_FAULT_NONE;
}

static void oracle_foldback(float *cut, float *resume)
{
    if (W->exp_fb_cut > 0.0f) { *cut = W->exp_fb_cut; *resume = W->exp_fb_cut - 3.0f; }
    else if (env->rref_kohm == 33) { *cut = 99.0f; *resume = 96.0f; }
    else { *cut = 102.0f; *resume = 99.0f; }
}

static void check_settings(void)
{
    RCHECK(pb_heater_is_faulted() == W->faulted, "fault latch differs from the oracle");
    RCHECK(pb_heater_is_inhibited() == W->inhibited, "inhibit differs from the oracle");
    RCHECK(pb_heater_get_target_c() == W->exp_target, "target differs from the oracle");
    RCHECK(pb_heater_get_max_target_c() == W->exp_max, "ceiling differs from the oracle");
    RCHECK(pb_heater_get_comms_timeout_ms() == W->exp_comms_ms, "comms timeout differs");
    RCHECK(pb_heater_get_fb_cut_c() == W->exp_fb_cut, "fb_cut differs from the oracle");
    RCHECK((int)pb_heater_get_method() == W->exp_method, "method differs from the oracle");
    RCHECK(W->exp_target <= W->exp_max, "target above the ceiling");
    RCHECK(W->exp_max >= PB_HEATER_MIN_TARGET_C && W->exp_max <= 70.0f, "ceiling outside [30, 70]");
    RCHECK(W->exp_comms_ms >= 10000 && W->exp_comms_ms <= 300000, "comms timeout outside [10 s, 5 min]");
    float cr = pb_heater_get_cool_release_c();
    RCHECK(cr >= 30.0f && cr <= 65.0f, "cool_release outside [30, 65]");
}

// The cause that a boot restores from the stored code (pb_heater_fault_decide).
static int nvs_restored_cause(void)
{
    int c = nvs_u("fault_code");
    return (c > PB_FAULT_NONE && c < PB_FAULT__COUNT) ? c : PB_FAULT_EMERGENCY;
}

static bool oracle_hazard(int code)
{
    return code == PB_FAULT_PTC_OVERTEMP || code == PB_FAULT_CHAMBER_OVERTEMP ||
           code == PB_FAULT_CHAMBER_SENSOR || code == PB_FAULT_PTC_SENSOR ||
           code == PB_FAULT_LINK_LOST;
}

// True if this latch must write NVS: NVS does not hold the latch yet, or a
// hazard cause replaces a persisted non-hazard cause (emergency_off).
static bool oracle_writes(pb_fault_reason_t code, bool persists)
{
    return persists && (W->exp_cause == PB_FAULT_NONE ||
                        (oracle_hazard(code) && !oracle_hazard(W->exp_cause)));
}

static void latch_oracle(pb_fault_reason_t code, bool persists)
{
    if (oracle_writes(code, persists)) W->exp_cause = code;
    W->faulted = true;
    W->exp_target = 0.0f;
    W->expect_persist = W->exp_cause != PB_FAULT_NONE;
    (void)code;
}

// --- first-heat oracle -------------------------------------------------------------
// From docs/SAFETY.md, "First-heat check": a window opens at the first relay-on
// of a heat with the PTC below 70.0 C and counts relay ON time only. From 12 s
// of ON time: pass if the PTC rise is >= 3.0 C and > 2 x the chamber rise. At
// 30 s of ON time without a pass: uncertain (report only) if the PTC rose
// >= 3.0 C, crossed if the chamber rose >= 5.0 C and >= 3.0 C more than the
// PTC, else no-rise. A window without a decision after 120 s of wall time
// closes without a result.

static bool fh_open(void) { return W->fh == PB_FH_ARMED || W->fh == PB_FH_EVAL; }
static bool fh_failed(void) { return W->fh == PB_FH_CROSSED || W->fh == PB_FH_NO_RISE; }

static void fh_request_store(int val, int32_t p, int32_t c)
{
    W->fh_stored = val;
    W->fh_pending = true;
    W->fh_store_val = val;
    W->fh_store_p = p;
    W->fh_store_c = c;
}

static bool fh_store_landed(void)
{
    if (nvs_u("fh_state") != W->fh_store_val) return false;
    if (W->fh_store_val == PB_FH_STORED_UNKNOWN) return true;
    return (int32_t)nvs_u("fh_ptc_dc") == W->fh_store_p && (int32_t)nvs_u("fh_ch_dc") == W->fh_store_c &&
           (uint32_t)nvs_u("fh_cfg") == fh_cfg_expected();
}

// One window step on the armed, unlatched path. Returns the fault to latch.
static pb_fault_reason_t oracle_fh_step(float ptc, float ch, bool relay_was_on)
{
    if (!fh_open()) return PB_FAULT_NONE;
    int64_t dt = env->now_us - W->fh_last;
    if (relay_was_on && dt > 0) W->fh_on_us += dt;
    W->fh_last = env->now_us;
    int32_t p = pb_heater_c_to_dc(ptc), c = pb_heater_c_to_dc(ch);
    int32_t pf = p < W->fh_pprev ? p : W->fh_pprev, cf = c < W->fh_cprev ? c : W->fh_cprev;
    W->fh_pprev = p;
    W->fh_cprev = c;
    if (pf > W->fh_ppeak) W->fh_ppeak = pf;
    if (cf > W->fh_cpeak) W->fh_cpeak = cf;
    W->fh_prise = W->fh_ppeak - W->fh_p0;
    W->fh_crise = W->fh_cpeak - W->fh_c0;
    int64_t on_ms = W->fh_on_us / 1000;
    if (on_ms >= 12000 && W->fh_prise >= 30 && W->fh_prise > 2 * W->fh_crise) {
        W->fh = PB_FH_PASS;
        W->fh_pass++;
        fh_request_store(PB_FH_STORED_PASS, W->fh_prise, W->fh_crise);
        return PB_FAULT_NONE;
    }
    if (on_ms >= 30000) {
        if (W->fh_prise >= 30) {
            W->fh = PB_FH_UNCERTAIN;
            W->fh_uncertain++;
            return PB_FAULT_NONE;
        }
        if (W->fh_crise >= 50 && W->fh_crise >= W->fh_prise + 30) {
            W->fh = PB_FH_CROSSED;
            fh_request_store(PB_FH_STORED_CROSSED, W->fh_prise, W->fh_crise);
            return PB_FAULT_SENSORS_CROSSED;
        }
        W->fh = PB_FH_NO_RISE;
        fh_request_store(PB_FH_STORED_NO_RISE, W->fh_prise, W->fh_crise);
        return PB_FAULT_HEATER_NO_RISE;
    }
    if (env->now_us - W->fh_t0 > 120000000) {
        W->fh = PB_FH_IDLE;
        W->fh_timeouts++;
    } else if (on_ms >= 12000) {
        W->fh = PB_FH_EVAL;
    }
    return PB_FAULT_NONE;
}

static void check_first_heat(void)
{
    pb_heater_first_heat_t f = fw_fh();
    RCHECK((int)f.state == W->fh, "first-heat state %d, oracle %d", (int)f.state, W->fh);
    RCHECK((int)f.stored == W->fh_stored, "first-heat stored %d, oracle %d", (int)f.stored, W->fh_stored);
    RCHECK(f.enforced, "first-heat not enforced in a production build");
    if (fh_open() || W->fh == PB_FH_PASS || fh_failed() || W->fh == PB_FH_UNCERTAIN) {
        if (W->fh != PB_FH_PASS || W->fh_windows > 0) {
            RCHECK(f.ptc_rise_dc == W->fh_prise && f.ch_rise_dc == W->fh_crise,
                   "first-heat rises %d/%d, oracle %d/%d", (int)f.ptc_rise_dc, (int)f.ch_rise_dc,
                   (int)W->fh_prise, (int)W->fh_crise);
        }
    }
    if (fh_open())
        RCHECK((int64_t)f.on_ms == W->fh_on_us / 1000, "first-heat ON time %u ms, oracle %lld ms",
               (unsigned)f.on_ms, (long long)(W->fh_on_us / 1000));
}

static void one_tick(void)
{
    int64_t dt_ms = chance(0.9) ? 500 : (int64_t)rnd_u(3000) + 1;
    model_advance((double)dt_ms / 1000.0);
    env->now_us += dt_ms * 1000;
    if (W->controller_present && chance(0.97)) {
        link_alive();
        W->last_link_us = env->now_us;
    }
    update_sensors();
    W->ticks++;

    check_settings();
    const bool pre_faulted = W->faulted;
    const float target = W->exp_target;
    const bool armed = target > 0.0f;
    const bool ptc_ok = env->ntc_status[PB_NTC_PTC] == PB_NTC_OK;
    const bool ch_ok = env->ntc_status[PB_NTC_CHAMBER] == PB_NTC_OK;
    const float ptc = env->ntc_temp[PB_NTC_PTC];
    const float ch = env->ntc_temp[PB_NTC_CHAMBER];
    const bool expired = (env->now_us - W->last_link_us) > (int64_t)W->exp_comms_ms * 1000;
    const pb_fault_reason_t trip = oracle_trip(ptc_ok, ptc, ch_ok, ch, armed, expired);
    const bool ext = isfinite(W->ext_ctrl);
    const float reg = ext ? W->ext_ctrl : ch;
    const bool relay_was_on = relay() == 1;

    // First-heat, in the order of the tick.
    if (W->fh_rerun) {
        W->fh_rerun = false;
        if (W->fh == PB_FH_PASS || W->fh == PB_FH_UNCERTAIN || fh_open()) {
            W->fh = PB_FH_IDLE;
            W->fh_on_us = 0;
            W->fh_prise = W->fh_crise = 0;
        }
    }
    pb_fault_reason_t fh_trip = PB_FAULT_NONE;
    const bool fh_was_open = fh_open();
    const bool fh_was_pending = W->fh_pending;
    const int fh_val_before = W->fh_store_val;
    if (trip != PB_FAULT_NONE) {
        if (fh_open()) { W->fh = PB_FH_IDLE; W->fh_hazard_aborts++; }   // the hazard wins
    } else if (pre_faulted || !armed) {
        if (fh_open()) W->fh = PB_FH_IDLE;
        if (!pre_faulted && (fh_failed() || W->fh == PB_FH_UNCERTAIN)) W->fh = PB_FH_IDLE;
    } else {
        if (fh_failed()) W->fh = PB_FH_IDLE;     // the fault was cleared (UNCERTAIN stays)
        fh_trip = oracle_fh_step(ptc, ch, relay_was_on);
    }
    const bool fh_new_store = W->fh_pending && (!fh_was_pending || W->fh_store_val != fh_val_before ||
                                                (fh_was_open && !fh_open()));

    tick();
    if (pre_faulted) W->faulted_ticks++;
    if (armed) W->armed_ticks++;

    if (trip != PB_FAULT_NONE) {
        RCHECK(relay() == 0, "trip %d did not turn the relay off in the same tick", (int)trip);
        RCHECK(pb_heater_is_faulted(), "trip %d did not latch", (int)trip);
        RCHECK(pb_heater_fault_code() == trip, "fault code %d, expected %d",
               (int)pb_heater_fault_code(), (int)trip);
        W->trips[trip]++;
        if (!pre_faulted && nvs_writes_work())
            RCHECK(nvs_u("fault_latch") == 1 && nvs_u("fault_code") == (int)trip,
                   "trip %d was not persisted although NVS works", (int)trip);
        // Also on top of a RAM-only latch (panic-off, inhibit, the boot
        // NVS_UNREADABLE latch), or replacing a persisted emergency_off cause.
        if (oracle_writes(trip, true) && nvs_writes_work())
            RCHECK(nvs_u("fault_latch") == 1 && nvs_u("fault_code") == (int)trip,
                   "trip %d was not persisted (NVS cause was %d)", (int)trip, W->exp_cause);
        latch_oracle(trip, true);
    } else if (fh_trip != PB_FAULT_NONE) {
        RCHECK(relay() == 0, "first-heat trip %d did not turn the relay off", (int)fh_trip);
        RCHECK(pb_heater_is_faulted() && pb_heater_fault_code() == fh_trip,
               "first-heat trip %d did not latch (code %d)", (int)fh_trip, (int)pb_heater_fault_code());
        if (nvs_writes_work())
            RCHECK(nvs_u("fault_latch") == 1 && nvs_u("fault_code") == (int)fh_trip,
                   "first-heat trip %d was not persisted although NVS works", (int)fh_trip);
        W->trips[fh_trip]++;
        if (fh_trip == PB_FAULT_SENSORS_CROSSED) W->fh_crossed++;
        else W->fh_norise++;
        latch_oracle(fh_trip, true);
    } else {
        if (pre_faulted || !armed) {
            W->fb = false;
            W->local = false;
        } else {
            float cut, resume;
            oracle_foldback(&cut, &resume);
            if (ptc >= cut) W->fb = true;
            else if (ptc < resume) W->fb = false;
            if (!ext) W->local = false;
            else if (ch >= 72.0f) W->local = true;
            else if (ch < 67.0f) W->local = false;
        }
        if (W->fb) W->fb_cut_ticks++;
        if (W->local) W->local_cut_ticks++;
    }

    // A window opens at the first relay-on of a heat (after the drive decision).
    if (relay() == 1 && W->fh == PB_FH_IDLE && pb_heater_c_to_dc(ptc) < 700) {
        W->fh = PB_FH_ARMED;
        W->fh_t0 = W->fh_last = env->now_us;
        W->fh_on_us = 0;
        W->fh_p0 = W->fh_pprev = W->fh_ppeak = pb_heater_c_to_dc(ptc);
        W->fh_c0 = W->fh_cprev = W->fh_cpeak = pb_heater_c_to_dc(ch);
        W->fh_prise = W->fh_crise = 0;
        W->fh_windows++;
    }
    check_first_heat();
    // A new result is written in the same tick when NVS works.
    if (fh_new_store && nvs_writes_work()) {
        RCHECK(fh_store_landed(), "first-heat result %d not written although NVS works", W->fh_store_val);
        W->fh_pending = false;
    }

    if (relay() == 1) {
        W->relay_on_ticks++;
        RCHECK(trip == PB_FAULT_NONE, "relay on with a trip condition");
        RCHECK(fh_trip == PB_FAULT_NONE, "relay on in a first-heat trip tick");
        RCHECK(!pre_faulted && !pb_heater_is_faulted(), "relay on while a fault is latched");
        RCHECK(armed, "relay on without a positive target");
        RCHECK(ptc_ok && ch_ok, "relay on with a sensor that is not OK");
        RCHECK(ptc < 105.0f && ch < 85.0f, "relay on above a cutoff");
        RCHECK(!expired, "relay on with a stale controller link");
        RCHECK(!W->fb, "relay on while the element foldback cuts");
        RCHECK(!(ext && W->local), "relay on while the local foldback cuts");
        RCHECK(reg < target, "relay on at or above the target");
        RCHECK(target <= W->exp_max && W->exp_max <= PB_HEATER_ABS_MAX_TARGET_C,
               "relay on with a target above the clamp");
    }
    if (W->exp_method == PB_HEATER_METHOD_BANGBANG && trip == PB_FAULT_NONE && fh_trip == PB_FAULT_NONE &&
        !pre_faulted && armed && !W->fb && !(ext && W->local) &&
        reg < target - PB_HEATER_HYSTERESIS_C - 0.01f) {
        W->liveness_checks++;
        RCHECK(relay() == 1, "bang-bang: relay off although heat is permitted and demanded");
    }
    if (W->exp_method == PB_HEATER_METHOD_PID && trip == PB_FAULT_NONE && fh_trip == PB_FAULT_NONE &&
        !pre_faulted && armed) {
        pb_heater_telemetry_t tel;
        pb_heater_get_telemetry(&tel);
        if (W->fb || (ext && W->local))
            RCHECK(tel.commanded_duty == 0.0f, "PID: duty %.3f while a foldback cuts",
                   (double)tel.commanded_duty);
        // pb_heater_pid_window_on(): a window with less than the dwell of OFF
        // time is fully on, one with less than the dwell of ON time is off.
        int64_t on_us = (int64_t)(tel.commanded_duty * (float)PB_HEATER_PID_WINDOW_US);
        if (tel.commanded_duty > 0.0f && PB_HEATER_PID_WINDOW_US - on_us < PB_HEATER_PID_MIN_DWELL_US) {
            W->pid_liveness_checks++;
            RCHECK(relay() == 1, "PID: relay off at duty %.3f", (double)tel.commanded_duty);
        }
        if (on_us < PB_HEATER_PID_MIN_DWELL_US)
            RCHECK(relay() == 0, "PID: relay on at duty %.3f", (double)tel.commanded_duty);
    }

    // Durability: a latch that must persist lands within 2 s of NVS recovery.
    if (W->expect_persist && nvs_writes_work() &&
        env->now_us - W->nvs_healthy_since_us > 2000000 &&
        (nvs_u("fault_latch") != 1 || nvs_restored_cause() != W->exp_cause))
        rfail(__LINE__, "pending fault persist did not land 2 s after NVS recovered");
    if (W->fh_pending && nvs_writes_work() && env->now_us - W->nvs_healthy_since_us > 2000000) {
        RCHECK(fh_store_landed(), "pending first-heat result %d did not land 2 s after NVS recovered",
               W->fh_store_val);
        W->fh_pending = false;
    }

    check_settings();
}

// --- actions -----------------------------------------------------------------------------

static void act_link_alive_impl(void) { link_alive(); W->last_link_us = env->now_us; }

static float rnd_temp_setting(void)
{
    switch (rnd_u(6)) {
    case 0: return NAN;
    case 1: return chance(0.5) ? INFINITY : -INFINITY;
    case 2: return 0.0f;
    case 3: return (float)rnd_f(-50.0, 300.0);
    default: return (float)rnd_f(25.0, 75.0);
    }
}

static void act_set_target(void)
{
    float t = rnd_temp_setting();
    esp_err_t r = set_target(t);
    if (!isfinite(t)) {
        RCHECK(r == ESP_ERR_INVALID_ARG, "non-finite target accepted");
        return;
    }
    float c = t < 0.0f ? 0.0f : t;
    if (c > W->exp_max) c = W->exp_max;
    if (W->faulted && c > 0.0f) {
        RCHECK(r == ESP_ERR_INVALID_STATE, "positive target accepted while faulted");
    } else {
        RCHECK(r == ESP_OK, "valid target rejected (%d)", r);
        W->exp_target = c;
    }
}

static void act_clear(void)
{
    const bool flash_latched = nvs_u("fault_latch") == 1;
    esp_err_t r = clear_fault();
    if (W->inhibited) {
        RCHECK(r == ESP_ERR_INVALID_STATE, "clear accepted while inhibited");
    } else if (W->nvs_write_bad) {
        RCHECK(r != ESP_OK, "clear reported success although the NVS write failed");
        RCHECK(pb_heater_is_faulted() == W->faulted, "a failed clear changed the RAM latch");
        W->clears_failed++;
        // A clear that fails part way must leave flash latched.
        if (flash_latched)
            RCHECK(nvs_u("fault_latch") == 1, "a failed clear left flash unlatched");
    } else {
        RCHECK(r == ESP_OK, "clear failed with working NVS (%d)", r);
        RCHECK(nvs_u("fault_latch") == 0, "clear did not persist");
        W->faulted = false;
        W->exp_target = 0.0f;
        W->expect_persist = false;
        W->exp_cause = PB_FAULT_NONE;
        W->clears_ok++;
    }
}

static void act_panic(void)
{
    unsigned writes = env->writes[PB_GPIO_RELAY];
    env->ctx = CTX_OTHER;
    pb_heater_request_panic_off(chance(0.5) ? "button" : NULL);
    env_check_quiescent();
    RCHECK(env->writes[PB_GPIO_RELAY] == writes, "panic-off request wrote the GPIO");
    latch_oracle(PB_FAULT_PANIC_OFF, false);
    W->panics++;
}

static void act_emergency(void)
{
    bool writes = oracle_writes(PB_FAULT_EMERGENCY, true);
    env->ctx = CTX_CONTROL;
    pb_heater_emergency_off("random emergency");
    env->ctx = CTX_OTHER;
    env_check_quiescent();
    RCHECK(relay() == 0, "emergency_off left the relay on");
    if (writes && nvs_writes_work())
        RCHECK(nvs_u("fault_latch") == 1, "emergency_off was not persisted");
    latch_oracle(PB_FAULT_EMERGENCY, true);
}

static void act_inhibit(void)
{
    if (!chance(0.2)) return;                  // rare: it holds until a reboot
    env->ctx = CTX_CONTROL;
    pb_heater_inhibit("random inhibit");
    env->ctx = CTX_OTHER;
    env_check_quiescent();
    RCHECK(relay() == 0, "inhibit left the relay on");
    latch_oracle(PB_FAULT_INHIBITED, false);
    W->inhibited = true;
}

static void act_set_max(void)
{
    float m = rnd_temp_setting();
    esp_err_t r = pb_heater_set_max_target_c(m);
    env_check_quiescent();
    if (!isfinite(m)) { RCHECK(r == ESP_ERR_INVALID_ARG, "non-finite max accepted"); return; }
    RCHECK(r == ESP_OK, "max rejected");
    if (m < 30.0f) m = 30.0f;
    if (m > 70.0f) m = 70.0f;
    W->exp_max = m;
    if (W->exp_target > m) W->exp_target = m;
}

static void act_set_comms(void)
{
    uint32_t ms;
    switch (rnd_u(4)) {
    case 0: ms = 0; break;
    case 1: ms = 0xFFFFFFFFu; break;
    case 2: ms = rnd_u(400000); break;
    default: ms = 10000 + rnd_u(60000); break;
    }
    RCHECK(pb_heater_set_comms_timeout_ms(ms) == ESP_OK, "comms timeout rejected");
    env_check_quiescent();
    if (ms < 10000) ms = 10000;
    if (ms > 300000) ms = 300000;
    W->exp_comms_ms = ms;
}

static void act_set_fb_cut(void)
{
    float c = chance(0.3) ? (float)rnd_f(88.0, 106.0) : rnd_temp_setting();
    esp_err_t r = pb_heater_set_fb_cut_c(c);
    env_check_quiescent();
    if (!isfinite(c)) { RCHECK(r == ESP_ERR_INVALID_ARG, "non-finite fb_cut accepted"); return; }
    RCHECK(r == ESP_OK, "fb_cut rejected");
    if (c <= 0.0f) c = 0.0f;
    else if (c < 90.0f) c = 90.0f;
    else if (c > 104.0f) c = 104.0f;
    W->exp_fb_cut = c;
}

static void act_set_method(void)
{
    int m = chance(0.8) ? (int)rnd_u(2) : (int)rnd_u(10);
    esp_err_t r = pb_heater_set_method((pb_heater_method_t)m);
    env_check_quiescent();
    if (m > 1) { RCHECK(r == ESP_ERR_INVALID_ARG, "invalid method accepted"); return; }
    RCHECK(r == ESP_OK, "method rejected");
    W->exp_method = m;
}

static void act_set_cool(void)
{
    float c = rnd_temp_setting();
    esp_err_t r = pb_heater_set_cool_release_c(c);
    env_check_quiescent();
    RCHECK(isfinite(c) ? r == ESP_OK : r == ESP_ERR_INVALID_ARG, "cool_release result");
}

static void act_ext_ctrl(void)
{
    float v = chance(0.4) ? NAN : (float)rnd_f(-20.0, 120.0);
    pb_heater_set_control_chamber_c(v);
    env_check_quiescent();
    W->ext_ctrl = v;
}

static void act_sensor_mode(void)
{
    int ch = (int)rnd_u(2);
    W->mode[ch] = chance(0.4) ? SM_NORMAL : (int)rnd_u(SM__COUNT);
    W->noise[ch] = (float)rnd_f(0.0, 4.0);
    W->stuck[ch] = chance(0.5) ? env->ntc_temp[ch] : (float)rnd_f(-40.0, 200.0);
    if (!isfinite(W->stuck[ch])) W->stuck[ch] = 25.0f;
    static const int drop[] = { PB_NTC_OPEN, PB_NTC_SHORT, PB_NTC_UNINIT };
    W->drop_status[ch] = drop[rnd_u(3)];
}

static void act_nvs_write_fault(void)
{
    if (W->nvs_write_bad) {
        env->nvs_fail_open_rw = env->nvs_fail_set = 0;
        env->nvs_fail_set_nth = 0;
        W->nvs_write_bad = false;
        W->nvs_healthy_since_us = env->now_us;
        return;
    }
    if (!chance(0.3)) return;                  // keep NVS mostly healthy
    switch (rnd_u(4)) {
    case 0: env->nvs_fail_open_rw = ESP_ERR_NVS_NOT_INITIALIZED; break;
    case 1: env->nvs_fail_set = ESP_FAIL; break;
    case 2: env->nvs_fail_set_nth = 1; break;  // the first set of each sequence fails
    default: env->nvs_fail_set_nth = 2; break; // a partial write: the second set fails
    }
    W->nvs_write_bad = true;
}

static void act_nvs_read_fault(void)
{
    if (W->nvs_read_bad) {
        env->nvs_fail_open_ro = env->nvs_fail_get = 0;
        W->nvs_read_bad = false;
    } else {
        if (chance(0.5)) env->nvs_fail_open_ro = ESP_FAIL;
        else env->nvs_fail_get = ESP_FAIL;
        W->nvs_read_bad = true;
    }
}

static void act_nvs_corrupt(void)
{
    // A hand-edited or corrupt stored setting. It takes effect at the next boot.
    switch (rnd_u(7)) {
    case 5: env_nvs_write("fh_state", HOST_NVS_U8, rnd_u(6)); break;
    case 6:
        if (chance(0.5)) env_nvs_write("fh_cfg", HOST_NVS_U32, (uint32_t)rnd64());
        else env_nvs_write("fh_cfg", HOST_NVS_U32, fh_cfg_expected());
        break;
    case 0: env_nvs_write("heat_max_c", HOST_NVS_U32, (uint32_t)rnd64()); break;
    case 1: env_nvs_write("heat_comms_ms", HOST_NVS_U32, (uint32_t)rnd64()); break;
    case 2: env_nvs_write("fb_cut_c", HOST_NVS_U32, rnd_u(30000)); break;
    case 3: env_nvs_write("heat_method", HOST_NVS_U8, rnd_u(256)); break;
    default: env_nvs_write("cool_rel_c", HOST_NVS_U32, (uint32_t)rnd64()); break;
    }
}

static void act_teleport(void)
{
    W->te = rnd_f(-20.0, 130.0);
    W->tc = rnd_f(-20.0, W->te > 100.0 ? 100.0 : W->te);
}

static void act_near_limits(void)
{
    // Put the true temperatures close to the foldback and cutoff thresholds.
    W->te = rnd_f(94.0, 108.0);
    W->tc = rnd_f(60.0, 88.0);
    if (chance(0.5)) {
        W->ext_ctrl = (float)rnd_f(30.0, 69.0);
        pb_heater_set_control_chamber_c(W->ext_ctrl);
    }
}

static void act_cooldown(void)
{
    // A long time with the relay off: both nodes are at ambient again.
    W->te = W->tc = W->ta;
}

static void act_advance(void)
{
    // Up to 6 min of ticks: more than the longest comms timeout.
    int n = chance(0.7) ? (int)rnd_u(20) + 1 : (int)rnd_u(720) + 1;
    for (int i = 1; i < n; ++i) one_tick();   // the step adds one more tick
}

typedef struct { const char *name; int weight; void (*fn)(void); } action_t;

static void act_operator(void)
{
    // What a user does after a fault: wait for it to cool, clear, start again.
    W->te = W->tc = W->ta;
    for (int ch = 0; ch < 2; ++ch) W->mode[ch] = SM_NORMAL;
    W->controller_present = true;
    act_link_alive_impl();
    if (W->faulted) act_clear();
    float t = (float)rnd_f(30.0, 70.0);
    esp_err_t r = set_target(t);
    if (!W->faulted) {
        RCHECK(r == ESP_OK, "operator target rejected");
        W->exp_target = t > W->exp_max ? W->exp_max : t;
    }
}

static void act_link_alive(void) { act_link_alive_impl(); }
static void act_controller(void) { W->controller_present = !W->controller_present; }
static void act_fan(void) { W->fan = !W->fan; }
static void act_bias(void)
{
    int ch = (int)rnd_u(2);
    W->bias[ch] = (float)rnd_f(-5.0, 5.0);
}
static void act_reboot(void) { /* handled by the caller */ }

static void act_plant(void)
{
    // Mostly a normal plant. The faults are what the first-heat check must find.
    W->plant = chance(0.5) ? PLANT_NORMAL : (int)rnd_u(PLANT__COUNT);
}

static void act_fh_rerun(void)
{
    env->ctx = CTX_OTHER;
    esp_err_t r = pb_heater_first_heat_rerun();
    env_check_quiescent();
    W->fh_rerun = true;
    fh_request_store(PB_FH_STORED_UNKNOWN, 0, 0);
    if (!W->nvs_write_bad) RCHECK(r == ESP_OK, "first-heat re-run failed with working NVS (%d)", r);
    if (r == ESP_OK) {
        RCHECK(nvs_u("fh_state") == PB_FH_STORED_UNKNOWN, "re-run reported success but NVS differs");
        W->fh_pending = false;
    }
}

static const action_t k_actions[] = {
    { "set_target",      14, act_set_target },
    { "operator",         6, act_operator },
    { "advance",         14, act_advance },
    { "controller",       5, act_controller },
    { "link_alive",       4, act_link_alive },
    { "sensor_mode",      8, act_sensor_mode },
    { "bias",             2, act_bias },
    { "panic",            3, act_panic },
    { "clear_fault",      8, act_clear },
    { "set_max",          3, act_set_max },
    { "set_comms",        3, act_set_comms },
    { "set_fb_cut",       3, act_set_fb_cut },
    { "set_method",       3, act_set_method },
    { "set_cool",         1, act_set_cool },
    { "ext_ctrl",         3, act_ext_ctrl },
    { "nvs_write_fault",  2, act_nvs_write_fault },
    { "nvs_read_fault",   1, act_nvs_read_fault },
    { "nvs_corrupt",      1, act_nvs_corrupt },
    { "fan",              2, act_fan },
    { "teleport",         1, act_teleport },
    { "cooldown",         3, act_cooldown },
    { "near_limits",      3, act_near_limits },
    { "emergency_off",    1, act_emergency },
    { "inhibit",          1, act_inhibit },
    { "reboot",           1, act_reboot },
    { "plant",            2, act_plant },
    { "fh_rerun",         3, act_fh_rerun },
};
#define N_ACTIONS ((int)(sizeof(k_actions) / sizeof(k_actions[0])))

// --- boot and the child process ----------------------------------------------------------

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void oracle_boot(void)
{
    // Expected settings: pb_heater_load_config() reads the committed NVS values
    // and clamps them. With an NVS read failure each value is the default.
    uint32_t v;
    bool rd = !W->nvs_read_bad && env->nvs_ns_exists;
    W->exp_max = (rd && env_nvs_read("heat_max_c", &v)) ? clampf((float)v / 100.0f, 30.0f, 70.0f) : 70.0f;
    W->exp_comms_ms = 300000;
    if (rd && env_nvs_read("heat_comms_ms", &v))
        W->exp_comms_ms = v < 10000 ? 10000 : (v > 300000 ? 300000 : v);
    W->exp_fb_cut = 0.0f;
    if (rd && env_nvs_read("fb_cut_c", &v) && v > 0)
        W->exp_fb_cut = clampf((float)v / 100.0f, 90.0f, 104.0f);
    W->exp_method = (rd && env_nvs_read("heat_method", &v) && v == 1) ? 1 : 0;

    // Expected latch: SAFETY.md, "Persistent fault latch".
    bool latched;
    if (env->nvs_fail_open_ro) latched = true;                 // unreadable
    else if (!env->nvs_ns_exists) latched = false;             // fresh device
    else if (env->nvs_fail_get) latched = true;                // unreadable
    else latched = env_nvs_read("fault_latch", &v) && v != 0;
    W->faulted = latched;
    W->inhibited = false;
    W->exp_target = 0.0f;
    // The firmware treats a latch it restored from NVS as written. After a read
    // failure (NVS_UNREADABLE) it treats the latch as not written.
    W->exp_cause = PB_FAULT_NONE;
    if (latched && !env->nvs_fail_open_ro && !env->nvs_fail_get) {
        W->exp_cause = nvs_restored_cause();
    }
    W->expect_persist = W->exp_cause != PB_FAULT_NONE;
    W->fb = W->local = false;
    W->ext_ctrl = NAN;

    // Expected first-heat state: a stored result counts only for the same
    // mapping and offsets. A read failure gives UNKNOWN (the check runs again).
    W->fh = PB_FH_IDLE;
    W->fh_rerun = false;
    W->fh_stored = PB_FH_STORED_UNKNOWN;
    W->fh_prise = W->fh_crise = 0;
    W->fh_on_us = 0;
    W->fh_pending = false;         // a write that did not land before the reboot is lost
    if (!env->nvs_fail_open_ro && env->nvs_ns_exists && !env->nvs_fail_get &&
        env_nvs_read("fh_state", &v) && v >= 1 && v <= 3) {
        uint32_t cfg;
        if (env_nvs_read("fh_cfg", &cfg) && cfg == fh_cfg_expected()) {
            W->fh_stored = (int)v;
            uint32_t r;
            W->fh_prise = env_nvs_read("fh_ptc_dc", &r) ? (int32_t)r : 0;
            W->fh_crise = env_nvs_read("fh_ch_dc", &r) ? (int32_t)r : 0;
            if (v == PB_FH_STORED_PASS) W->fh = PB_FH_PASS;
        } else {
            fh_request_store(PB_FH_STORED_UNKNOWN, 0, 0);   // stale: written at boot
        }
    }
    W->last_link_us = env->now_us;
    W->nvs_healthy_since_us = env->now_us;
}

static void random_child(void)
{
    env->level[PB_GPIO_RELAY] = (int)rnd_u(2);   // undefined pin state before init
    g_fh_seed_pass = false;
    oracle_boot();
    boot();
    RCHECK(relay() == 0, "relay not off after boot");
    check_settings();
    if (W->fh_pending && nvs_writes_work()) {
        RCHECK(fh_store_landed(), "stale first-heat result not reset at boot");
        W->fh_pending = false;
    }
    check_first_heat();

    int total = 0;
    for (int i = 0; i < N_ACTIONS; ++i) total += k_actions[i].weight;
    while (W->step < W->steps) {
        W->step++;
        int pick = (int)rnd_u((uint32_t)total), i = 0;
        while (pick >= k_actions[i].weight) pick -= k_actions[i++].weight;
        W->action = k_actions[i].name;
        if (k_actions[i].fn == act_reboot) {
            W->reboots++;
            fflush(stdout);
            exit(REBOOT_EXIT_CODE);
        }
        k_actions[i].fn();
        check_settings();
        one_tick();
    }
}

static long env_long(const char *name, long def)
{
    const char *s = getenv(name);
    return (s && *s) ? strtol(s, NULL, 0) : def;
}

typedef struct {
    long relay_on, liveness, pid_liveness, fb_cut, local_cut;
    long trips[PB_FAULT__COUNT];
    long fh_windows, fh_pass, fh_crossed, fh_norise, fh_uncertain, fh_hazard_aborts;
} totals_t;

static bool run_seed(uint64_t seed, long steps, totals_t *tot)
{
    memset(W, 0, sizeof(*W));
    W->seed = seed;
    W->rng = seed;
    W->steps = steps;
    env_reset();
    static const int rrefs[] = { 82, 33, 82, 33, 47, 0 };
    env->rref_kohm = rrefs[rnd_u(6)];

    W->ta = rnd_f(5.0, 45.0);
    W->te = W->tc = W->ta;
    W->pmax_w = rnd_f(50.0, 400.0);
    W->ce_j_k = rnd_f(100.0, 800.0);
    W->cc_j_k = rnd_f(500.0, 8000.0);
    W->kec_w_k = rnd_f(2.0, 20.0);
    W->kca_w_k = rnd_f(1.0, 10.0);
    W->fan_boost = rnd_f(0.0, 5.0);
    W->knee_c = rnd_f(70.0, 180.0);
    W->knee_w = rnd_f(2.0, 20.0);
    for (int ch = 0; ch < 2; ++ch) W->bias[ch] = (float)rnd_f(-5.0, 5.0);
    W->controller_present = true;

    int code;
    while ((code = env_run_code(random_child)) == REBOOT_EXIT_CODE) {}
    if (code != 0) {
        fprintf(stderr, "FAIL: random seed %" PRIu64 " (exit code %d)\n", seed, code);
        return false;
    }
    printf("ok   random seed=%" PRIu64 " rref=%d steps=%ld ticks=%ld faulted=%ld armed=%ld relay_on=%ld "
           "fb_cut=%ld local_cut=%ld reboots=%ld panics=%ld clears=%ld/%ld "
           "liveness=%ld pid_liveness=%ld trips[ptc=%ld ch=%ld chs=%ld ptcs=%ld link=%ld]\n",
           seed, env->rref_kohm, W->steps, W->ticks, W->faulted_ticks, W->armed_ticks,
           W->relay_on_ticks,
           W->fb_cut_ticks, W->local_cut_ticks, W->reboots, W->panics,
           W->clears_ok, W->clears_failed, W->liveness_checks, W->pid_liveness_checks,
           W->trips[PB_FAULT_PTC_OVERTEMP], W->trips[PB_FAULT_CHAMBER_OVERTEMP],
           W->trips[PB_FAULT_CHAMBER_SENSOR], W->trips[PB_FAULT_PTC_SENSOR],
           W->trips[PB_FAULT_LINK_LOST]);
    tot->relay_on += W->relay_on_ticks;
    tot->liveness += W->liveness_checks;
    tot->pid_liveness += W->pid_liveness_checks;
    tot->fb_cut += W->fb_cut_ticks;
    tot->local_cut += W->local_cut_ticks;
    for (int i = 0; i < PB_FAULT__COUNT; ++i) tot->trips[i] += W->trips[i];
    printf("     first-heat windows=%ld pass=%ld crossed=%ld no_rise=%ld uncertain=%ld hazard_aborts=%ld "
           "timeouts=%ld\n", W->fh_windows, W->fh_pass, W->fh_crossed, W->fh_norise, W->fh_uncertain,
           W->fh_hazard_aborts, W->fh_timeouts);
    tot->fh_windows += W->fh_windows;
    tot->fh_pass += W->fh_pass;
    tot->fh_crossed += W->fh_crossed;
    tot->fh_norise += W->fh_norise;
    tot->fh_uncertain += W->fh_uncertain;
    tot->fh_hazard_aborts += W->fh_hazard_aborts;
    return true;
}

// The default seeds must reach each safety path at least once, so that a
// change in the generator cannot make the test vacuous without notice.
static bool check_totals(const totals_t *t)
{
    struct { const char *what; long n; } req[] = {
        { "relay-on ticks", t->relay_on },
        { "bang-bang liveness checks", t->liveness },
        { "PID liveness checks", t->pid_liveness },
        { "element foldback cuts", t->fb_cut },
        { "local foldback cuts", t->local_cut },
        { "PTC over-temp trips", t->trips[PB_FAULT_PTC_OVERTEMP] },
        { "chamber over-temp trips", t->trips[PB_FAULT_CHAMBER_OVERTEMP] },
        { "chamber sensor trips", t->trips[PB_FAULT_CHAMBER_SENSOR] },
        { "PTC sensor trips", t->trips[PB_FAULT_PTC_SENSOR] },
        { "link-lost trips", t->trips[PB_FAULT_LINK_LOST] },
        { "first-heat windows", t->fh_windows },
        { "first-heat passes", t->fh_pass },
        { "first-heat crossed results", t->fh_crossed },
        { "first-heat no-rise results", t->fh_norise },
        { "first-heat uncertain results", t->fh_uncertain },
        { "first-heat windows aborted by a hazard trip", t->fh_hazard_aborts },
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(req) / sizeof(req[0]); ++i) {
        if (req[i].n < 1) {
            fprintf(stderr, "random test coverage too low: 0 %s over all seeds\n", req[i].what);
            ok = false;
        }
    }
    return ok;
}

bool heater_random_test(void)
{
    totals_t tot;
    memset(&tot, 0, sizeof(tot));
    void *p = mmap(NULL, sizeof(world_t), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return false; }
    W = p;

    long steps = env_long("PB_HEATER_STEPS", 4000);
    const char *one = getenv("PB_HEATER_SEED");
    bool ok = true;
    if (one && *one) {
        uint64_t seed = strtoull(one, NULL, 0);
        printf("random test: PB_HEATER_SEED=%" PRIu64 " steps=%ld\n", seed, steps);
        ok = run_seed(seed, steps, &tot);
    } else {
        long n = env_long("PB_HEATER_SEEDS", 8);
        printf("random test: %ld seeds from 0xDB000001, steps=%ld "
               "(override: PB_HEATER_SEED, PB_HEATER_SEEDS, PB_HEATER_STEPS)\n", n, steps);
        for (long i = 0; i < n; ++i) ok &= run_seed(0xDB000001ull + (uint64_t)i, steps, &tot);
        // Only the full default set must reach every path.
        if (ok && !getenv("PB_HEATER_SEEDS") && !getenv("PB_HEATER_STEPS")) ok = check_totals(&tot);
    }
    munmap(p, sizeof(world_t));
    return ok;
}
