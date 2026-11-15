// SPDX-License-Identifier: MIT
// Directed tests of the first-heat check in the REAL pb_heater_tick() (see
// docs/SAFETY.md, "First-heat check"). Linked into the tick host test binary.
// The plant here is a simple ramp: each 500 ms tick adds a fixed step to the
// PTC and chamber readings.
#define _DEFAULT_SOURCE
#include "pb_heater.h"
#include "pb_heater_tick_host_test.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static pb_heater_first_heat_t fh(void)
{
    pb_heater_first_heat_t f;
    env->ctx = CTX_OTHER;
    pb_heater_get_first_heat(&f);
    env_check_quiescent();
    return f;
}

static void fresh_boot(void)
{
    g_fh_seed_pass = false;
    boot();
}

// Start a heat from (chamber, ptc). The first relay-on opens the window.
static void start(float chamber, float ptc, float target)
{
    sensors(chamber, ptc);
    link_alive();
    CHECK(set_target(target) == ESP_OK);
    tick();
    CHECK(relay() == 1);
}

// One 500 ms tick with a ramp of the readings.
static void step(float dch, float dptc)
{
    advance_ms(500);
    env->ntc_temp[PB_NTC_CHAMBER] += dch;
    env->ntc_temp[PB_NTC_PTC] += dptc;
    link_alive();
    tick();
}

// Tick a ramp until the first-heat window closes or a fault latches. Returns
// the number of ticks.
static int run_window(float dch, float dptc, int max_ticks)
{
    int n = 0;
    while (n < max_ticks) {
        step(dch, dptc);
        n++;
        pb_fh_state_t s = fh().state;
        if ((s != PB_FH_ARMED && s != PB_FH_EVAL) || pb_heater_is_faulted()) break;
    }
    return n;
}

// --- a normal plant -----------------------------------------------------------------

static void t_fh_pass_phase1(void)
{
    fresh_boot();
    CHECK(fh().state == PB_FH_IDLE && fh().stored == PB_FH_STORED_UNKNOWN);
    CHECK(fh().enforced);
    start(25.0f, 25.0f, 50.0f);
    CHECK(fh().state == PB_FH_ARMED);
    // No decision before 12 s of ON time, also with a large rise.
    for (int i = 0; i < 23; ++i) { step(0.1f, 1.0f); CHECK(fh().state == PB_FH_ARMED); }
    step(0.1f, 1.0f);                      // 24 x 500 ms = 12 s of ON time
    pb_heater_first_heat_t f = fh();
    CHECK(f.state == PB_FH_PASS);
    CHECK(f.on_ms == 12000);
    // The start is the median of the first three readings (26 C). The peak of
    // min(this, last) lags one sample: 48 - 26 = 22 C.
    CHECK(f.ptc_rise_dc == 220 && f.ch_rise_dc >= 21 && f.ch_rise_dc <= 23);
    CHECK(relay() == 1 && !pb_heater_is_faulted());
    CHECK(f.stored == PB_FH_STORED_PASS);
    CHECK(nvs_u("fh_state") == PB_FH_STORED_PASS);
    CHECK(nvs_u("fh_ptc_dc") == 220 && nvs_u("fh_ch_dc") == f.ch_rise_dc);
    CHECK((uint32_t)nvs_u("fh_cfg") == fh_cfg_expected());
}

static void t_fh_pass_phase2_skips(void)
{
    // The stored pass skips the check: a plant without a rise heats on.
    boot();
    CHECK(fh().state == PB_FH_PASS && fh().stored == PB_FH_STORED_PASS);
    CHECK(fh().ptc_rise_dc == 220);
    start(25.0f, 25.0f, 50.0f);
    for (int i = 0; i < 200; ++i) { step(0.0f, 0.0f); CHECK(relay() == 1); }
    CHECK(!pb_heater_is_faulted() && fh().state == PB_FH_PASS);
}

static void t_fh_pass_phase3_offset_resets(void)
{
    // A changed offset (or mapping) makes the stored pass invalid at boot.
    env->ntc_offset_c[PB_NTC_PTC] = 1.5f;
    boot();
    CHECK(fh().state == PB_FH_IDLE && fh().stored == PB_FH_STORED_UNKNOWN);
    CHECK(nvs_u("fh_state") == PB_FH_STORED_UNKNOWN);
}

static void t_fh_pass_pid(void)
{
    fresh_boot();
    CHECK(pb_heater_set_method(PB_HEATER_METHOD_PID) == ESP_OK);
    start(25.0f, 25.0f, 60.0f);
    run_window(0.1f, 1.0f, 100);
    CHECK(fh().state == PB_FH_PASS && !pb_heater_is_faulted());
}

// --- crossed roles ------------------------------------------------------------------

// Crossed leads: the "chamber" channel reads the element, the "PTC" channel the
// outlet air, which also rises. The chamber rise is more than 2 x the PTC rise
// + 2 C, and above 10 C at 12 s of ON time: WRONG_ROLES at 12 s.
static void t_fh_crossed_phase1(void)
{
    fresh_boot();
    start(25.0f, 25.0f, 70.0f);
    for (int i = 0; i < 23; ++i) {
        step(0.5f, 0.15f);
        CHECK(!pb_heater_is_faulted() && relay() == 1);   // no result before 12 s
    }
    unsigned high = env->high_writes[PB_GPIO_RELAY];
    step(0.5f, 0.15f);
    CHECK(relay() == 0);
    CHECK(env->high_writes[PB_GPIO_RELAY] == high);
    CHECK(pb_heater_is_faulted());
    CHECK(pb_heater_fault_code() == PB_FAULT_SENSOR_ROLES);
    CHECK(strcmp(pb_heater_fault_reason(),
                 "sensor roles wrong or PTC sensor not on element (first-heat check)") == 0);
    CHECK(pb_heater_get_target_c() == 0.0f);
    CHECK(fh().state == PB_FH_WRONG_ROLES && fh().stored == PB_FH_STORED_WRONG_ROLES);
    CHECK(fh().ptc_rise_dc >= 30);                  // the outlet air rose: no absolute PTC limit
    CHECK(fh().ch_rise_dc >= 2 * fh().ptc_rise_dc + PB_FH_ROLES_MARGIN_DC);
    CHECK(nvs_u("fault_latch") == 1 && nvs_u("fault_code") == PB_FAULT_SENSOR_ROLES);
    CHECK(nvs_u("fh_state") == PB_FH_STORED_WRONG_ROLES);
    // Latched: cool readings do not bring heat back, a new target is refused.
    for (int i = 0; i < 10; ++i) { step(0.0f, 0.0f); CHECK(relay() == 0); }
    CHECK(set_target(50.0f) == ESP_ERR_INVALID_STATE);
    CHECK(fh().state == PB_FH_WRONG_ROLES);
    // The check never swaps the channels (pb_ntc_set_swap_thermistors is not
    // even linked into this test).
    CHECK(!env->ntc_swap_active);
    // The clear brings the session back to IDLE. The stored hint stays.
    CHECK(clear_fault() == ESP_OK);
    tick();
    CHECK(fh().state == PB_FH_IDLE && fh().stored == PB_FH_STORED_WRONG_ROLES);
    CHECK(nvs_u("fh_state") == PB_FH_STORED_WRONG_ROLES);
}

static void t_fh_crossed_phase2_swap_applied(void)
{
    // The user applies the swap in Settings. It takes effect at the reboot: the
    // stored hint is for the old mapping and is reset.
    env->ntc_swap_active = true;
    boot();
    CHECK(!pb_heater_is_faulted());              // phase 1 cleared it
    CHECK(fh().stored == PB_FH_STORED_UNKNOWN);
    CHECK(nvs_u("fh_state") == PB_FH_STORED_UNKNOWN);
    start(25.0f, 25.0f, 50.0f);
    run_window(0.1f, 1.0f, 100);
    CHECK(fh().state == PB_FH_PASS);
    CHECK((uint32_t)nvs_u("fh_cfg") == fh_cfg_expected());
}

static void t_fh_crossed_restored(void)
{
    // A wrong-roles result latches over a power cycle like a hazard trip.
    fresh_boot();
    start(25.0f, 25.0f, 70.0f);
    run_window(0.5f, 0.0f, 100);
    CHECK(pb_heater_fault_code() == PB_FAULT_SENSOR_ROLES);
    boot();
    CHECK(pb_heater_is_faulted() && pb_heater_fault_code() == PB_FAULT_SENSOR_ROLES);
    CHECK(fh().stored == PB_FH_STORED_WRONG_ROLES && fh().state == PB_FH_IDLE);
    CHECK(set_target(50.0f) == ESP_ERR_INVALID_STATE);
}

// --- no rise --------------------------------------------------------------------------

static void t_fh_dead_phase1(void)
{
    fresh_boot();
    start(25.0f, 25.0f, 50.0f);
    for (int i = 0; i < 59; ++i) {
        step(0.0f, 0.0f);
        CHECK(!pb_heater_is_faulted() && relay() == 1);
        CHECK(fh().state == (i < 23 ? PB_FH_ARMED : PB_FH_EVAL));
    }
    step(0.0f, 0.0f);                          // 30 s of ON time
    CHECK(relay() == 0);
    CHECK(pb_heater_is_faulted() && pb_heater_fault_code() == PB_FAULT_HEATER_NO_RISE);
    CHECK(strcmp(pb_heater_fault_reason(), "heater no-rise (first-heat check)") == 0);
    CHECK(fh().state == PB_FH_NO_RISE && fh().on_ms == 30000);
    CHECK(nvs_u("fault_latch") == 1 && nvs_u("fault_code") == PB_FAULT_HEATER_NO_RISE);
    CHECK(nvs_u("fh_state") == PB_FH_STORED_NO_RISE);
}

static void t_fh_dead_phase2_still_latched(void)
{
    env->level[PB_GPIO_RELAY] = 1;
    boot();
    CHECK(pb_heater_is_faulted() && pb_heater_fault_code() == PB_FAULT_HEATER_NO_RISE);
    CHECK(strcmp(pb_heater_fault_reason(), "heater no-rise (first-heat check)") == 0);
    CHECK(fh().stored == PB_FH_STORED_NO_RISE);
    CHECK(set_target(40.0f) == ESP_ERR_INVALID_STATE);
    sensors(25.0f, 25.0f);
    for (int i = 0; i < 10; ++i) { step(0.0f, 0.0f); CHECK(relay() == 0); }
    // Clear, heat again: the check runs again and latches again.
    CHECK(clear_fault() == ESP_OK);
    start(25.0f, 25.0f, 50.0f);
    CHECK(fh().state == PB_FH_ARMED);
    run_window(0.0f, 0.0f, 100);
    CHECK(pb_heater_fault_code() == PB_FAULT_HEATER_NO_RISE);
}

static void t_fh_detached_ptc(void)
{
    // The PTC thermistor is off the element: it reads the room. The chamber
    // warms a little: below the WRONG_ROLES margin, so NO_RISE.
    fresh_boot();
    start(25.0f, 25.0f, 50.0f);
    run_window(0.02f, 0.0f, 100);
    CHECK(pb_heater_fault_code() == PB_FAULT_HEATER_NO_RISE);
    CHECK(fh().ch_rise_dc >= 10 && fh().ch_rise_dc <= 12 && fh().ptc_rise_dc == 0);
}

static void t_fh_both_rise_uncertain(void)
{
    // Both channels rise a lot (the outlet air follows the element closely).
    // That is not a pass, but never CROSSED: UNCERTAIN reports only.
    fresh_boot();
    start(25.0f, 25.0f, 70.0f);
    run_window(0.5f, 0.4f, 100);
    CHECK(!pb_heater_is_faulted() && relay() == 1);
    CHECK(fh().state == PB_FH_UNCERTAIN && fh().on_ms == 30000);
    CHECK(fh().ch_rise_dc > fh().ptc_rise_dc);      // the old rule said CROSSED here
    CHECK(fh().stored == PB_FH_STORED_UNKNOWN && nvs_u("fh_state") == -1);
    for (int i = 0; i < 10; ++i) { step(0.0f, 0.0f); CHECK(relay() == 1); }
    CHECK(fh().state == PB_FH_UNCERTAIN);
    CHECK(fh().uncertain_streak == 1);
    // The next heat checks again. A repeated UNCERTAIN counts up; another
    // result sets the streak to 0.
    CHECK(set_target(0.0f) == ESP_OK);
    tick();
    CHECK(fh().state == PB_FH_IDLE);
    start(25.0f, 25.0f, 70.0f);
    CHECK(fh().state == PB_FH_ARMED);
    run_window(0.5f, 0.4f, 100);
    CHECK(fh().state == PB_FH_UNCERTAIN && fh().uncertain_streak == 2);
    CHECK(set_target(0.0f) == ESP_OK);
    tick();
    start(25.0f, 25.0f, 50.0f);
    run_window(0.1f, 1.0f, 100);
    CHECK(fh().state == PB_FH_PASS && fh().uncertain_streak == 0);
}

static void t_fh_small_ptc_lead_uncertain(void)
{
    // The PTC rose 3 C or more but not more than 2 x the chamber: UNCERTAIN.
    fresh_boot();
    start(25.0f, 25.0f, 50.0f);
    run_window(0.05f, 0.08f, 100);
    CHECK(!pb_heater_is_faulted() && fh().state == PB_FH_UNCERTAIN);
    CHECK(fh().ptc_rise_dc >= 30 && fh().ptc_rise_dc <= 2 * fh().ch_rise_dc);
}

// The margins of WRONG_ROLES. Similar rises are never WRONG_ROLES.
static void roles_case(float dch, float dptc, pb_fault_reason_t expect, pb_fh_state_t state)
{
    fresh_boot();
    if (pb_heater_is_faulted()) CHECK(clear_fault() == ESP_OK);
    start(25.0f, 25.0f, 70.0f);
    run_window(dch, dptc, 100);
    CHECKF(pb_heater_fault_code() == expect && fh().state == state,
           "ramp %.3f/%.3f: code %d state %d (rises %d/%d)", (double)dch, (double)dptc,
           (int)pb_heater_fault_code(), (int)fh().state, (int)fh().ptc_rise_dc, (int)fh().ch_rise_dc);
}

static void t_fh_roles_margins(void)
{
    // At 30 s: chamber 5.8 C, PTC 1.2 C: 5.8 >= 2 x 1.2 + 2 -> WRONG_ROLES.
    roles_case(0.1f, 0.02f, PB_FAULT_SENSOR_ROLES, PB_FH_WRONG_ROLES);
    // Chamber 3.4 C, PTC 1.2 C: below the margin, the PTC is flat -> NO_RISE.
    roles_case(0.06f, 0.02f, PB_FAULT_HEATER_NO_RISE, PB_FH_NO_RISE);
    // Crossed leads with fast outlet air: chamber 34.8 C, PTC 17.4 C. Not more
    // than 2 x + 2 C: the ambiguous band, UNCERTAIN (no latch).
    roles_case(0.6f, 0.3f, PB_FAULT_NONE, PB_FH_UNCERTAIN);
    // Chamber 46 C, PTC 17.4 C: clear lead at 30 s -> WRONG_ROLES.
    roles_case(0.8f, 0.3f, PB_FAULT_SENSOR_ROLES, PB_FH_WRONG_ROLES);
}

static void t_fh_glitch(void)
{
    // One spike sample on a dead element is not a rise.
    fresh_boot();
    start(25.0f, 25.0f, 50.0f);
    for (int i = 0; i < 30; ++i) step(0.0f, 0.0f);
    step(0.0f, 20.0f);
    step(0.0f, -20.0f);
    CHECK(fh().ptc_rise_dc == 0);
    run_window(0.0f, 0.0f, 100);
    CHECK(pb_heater_fault_code() == PB_FAULT_HEATER_NO_RISE);
}

// --- ON time, stop, timeout ------------------------------------------------------

static void t_fh_relay_off_not_counted(void)
{
    fresh_boot();
    start(30.0f, 25.0f, 50.0f);
    for (int i = 0; i < 12; ++i) step(0.0f, 0.0f);         // 6 s ON
    CHECK(fh().on_ms == 6000);
    CHECK(set_target(29.0f) == ESP_OK);                      // target reached: relay off
    for (int i = 0; i < 40; ++i) {                           // 20 s OFF
        step(0.0f, 0.0f);
        CHECK(relay() == 0 && !pb_heater_is_faulted());
    }
    CHECK(fh().state == PB_FH_ARMED);
    CHECK(fh().on_ms == 6500);                               // the ON interval of the off tick
    CHECK(set_target(50.0f) == ESP_OK);                      // relay on again
    int64_t t_on = env->now_us;
    while (!pb_heater_is_faulted()) {
        step(0.0f, 0.0f);
        CHECK(env->now_us - t_on <= 30000000);
        if (!pb_heater_is_faulted()) CHECK(fh().on_ms < 30000);
    }
    CHECK(pb_heater_fault_code() == PB_FAULT_HEATER_NO_RISE);
    CHECK(fh().on_ms == 30000);
}

static void t_fh_low_duty_never_fails(void)
{
    // ON pulses at a low duty do not add up to a rise: no failed result after
    // 60 s of wall time. The window closes at 300 s (a restart).
    fresh_boot();
    start(30.0f, 25.0f, 50.0f);
    int64_t t0 = env->now_us;
    for (int cycle = 0; cycle < 30; ++cycle) {               // 2.5 s ON, 7.5 s OFF
        CHECK(set_target(50.0f) == ESP_OK);
        for (int i = 0; i < 5; ++i) step(0.0f, 0.0f);
        CHECK(set_target(29.0f) == ESP_OK);
        for (int i = 0; i < 15; ++i) step(0.0f, 0.0f);
        CHECK(!pb_heater_is_faulted());
    }
    CHECK(env->now_us - t0 == 300000000);
    CHECK(fh().on_ms >= 30000 && fh().state == PB_FH_EVAL);
    step(0.0f, 0.0f);
    CHECK(fh().state == PB_FH_IDLE && fh().restarts == 1 && !pb_heater_is_faulted());
}

static void t_fh_wall_timeout(void)
{
    // A window without a result closes after 300 s. The next relay OFF->ON edge
    // opens a new one with new start readings.
    fresh_boot();
    start(30.0f, 25.0f, 50.0f);
    int64_t t0 = env->now_us;
    for (int i = 0; i < 12; ++i) step(0.0f, 0.0f);
    CHECK(set_target(29.0f) == ESP_OK);                      // relay off
    while (fh().state == PB_FH_ARMED) {
        CHECK(env->now_us - t0 <= 300000000);
        step(0.0f, 0.0f);
    }
    CHECK(env->now_us - t0 == 300500000);
    CHECK(fh().state == PB_FH_IDLE && !pb_heater_is_faulted());
    CHECK(fh().restarts == 1);
    CHECK(set_target(50.0f) == ESP_OK);
    step(0.0f, 0.0f);
    CHECK(relay() == 1 && fh().state == PB_FH_ARMED && fh().on_ms == 0);
}

static void t_fh_stop_aborts(void)
{
    fresh_boot();
    start(25.0f, 25.0f, 50.0f);
    for (int i = 0; i < 40; ++i) step(0.0f, 0.0f);         // 20 s ON, no decision
    CHECK(fh().state == PB_FH_EVAL);
    CHECK(set_target(0.0f) == ESP_OK);
    tick();
    CHECK(relay() == 0 && !pb_heater_is_faulted());
    CHECK(fh().state == PB_FH_IDLE);
    // A new heat starts a new window from zero ON time.
    start(25.0f, 25.0f, 50.0f);
    CHECK(fh().state == PB_FH_ARMED && fh().on_ms == 0);
    for (int i = 0; i < 59; ++i) { step(0.0f, 0.0f); CHECK(!pb_heater_is_faulted()); }
    step(0.0f, 0.0f);
    CHECK(pb_heater_fault_code() == PB_FAULT_HEATER_NO_RISE);
}

static void t_fh_panic_aborts(void)
{
    fresh_boot();
    start(25.0f, 25.0f, 50.0f);
    for (int i = 0; i < 30; ++i) step(0.0f, 0.0f);
    pb_heater_request_panic_off("button");
    tick();
    CHECK(relay() == 0 && pb_heater_fault_code() == PB_FAULT_PANIC_OFF);
    CHECK(fh().state == PB_FH_IDLE);
}

static void t_fh_clear_and_target_before_tick(void)
{
    // A clear and a new target between two ticks must not skip the check.
    fresh_boot();
    start(25.0f, 25.0f, 50.0f);
    run_window(0.0f, 0.0f, 100);
    CHECK(pb_heater_fault_code() == PB_FAULT_HEATER_NO_RISE);
    CHECK(clear_fault() == ESP_OK);
    CHECK(set_target(50.0f) == ESP_OK);
    link_alive();
    tick();
    CHECK(relay() == 1 && fh().state == PB_FH_ARMED);
    run_window(0.0f, 0.0f, 100);
    CHECK(pb_heater_fault_code() == PB_FAULT_HEATER_NO_RISE);
}

static void edge(float chamber, float ptc)
{
    CHECK(set_target(29.0f) == ESP_OK);
    sensors(chamber, ptc);
    step(0.0f, 0.0f);
    CHECK(relay() == 0);
    CHECK(set_target(50.0f) == ESP_OK);
    step(0.0f, 0.0f);
    CHECK(relay() == 1);
}

static void t_fh_hot_start(void)
{
    // An element at 70 C or more, or 10 C or more above the chamber, can be at
    // its equilibrium: no window opens on that relay-on edge.
    fresh_boot();
    start(30.0f, 75.0f, 50.0f);
    CHECK(fh().state == PB_FH_IDLE && fh().skipped == 1);
    // Relay on all the time: no new edge, no window.
    for (int i = 0; i < 200; ++i) { step(0.0f, 0.0f); CHECK(relay() == 1); }
    CHECK(fh().state == PB_FH_IDLE && !pb_heater_is_faulted());
    edge(30.0f, 69.9f);                         // the reviewer's case: equilibrium near 70 C
    CHECK(fh().state == PB_FH_IDLE && fh().skipped == 2);
    edge(30.0f, 40.0f);                         // 10.0 C above the chamber
    CHECK(fh().state == PB_FH_IDLE && fh().skipped == 3);
    edge(30.0f, 39.9f);
    CHECK(fh().state == PB_FH_ARMED);
    // An element at its equilibrium just below the warm band does not rise.
    // That case is out of spec (equilibrium < 10 C above the air): NO_RISE.
    run_window(0.0f, 0.0f, 100);
    CHECK(pb_heater_fault_code() == PB_FAULT_HEATER_NO_RISE);
}

static void t_fh_start_glitch(void)
{
    // A low glitch in the first reading of a window must not give a dead
    // element a rise (the start is the median of three readings).
    fresh_boot();
    start(25.0f, 5.0f, 50.0f);                  // the PTC reading at the edge glitches low
    sensors(25.0f, 25.0f);
    run_window(0.0f, 0.0f, 100);
    CHECK(pb_heater_fault_code() == PB_FAULT_HEATER_NO_RISE);
    CHECK(fh().ptc_rise_dc == 0 && nvs_u("fh_state") == PB_FH_STORED_NO_RISE);
}

static void t_fh_offset_change_restarts(void)
{
    // An offset change moves the readings: the open window closes.
    fresh_boot();
    start(25.0f, 25.0f, 50.0f);
    for (int i = 0; i < 10; ++i) step(0.0f, 1.0f);
    env->ntc_offset_c[PB_NTC_CHAMBER] = -3.0f;
    step(0.0f, 1.0f);
    CHECK(fh().state == PB_FH_IDLE && fh().restarts == 1 && !pb_heater_is_faulted());
}

static void relay_low_at_result_write(const char *key)
{
    if (strcmp(key, "fh_state") == 0) CHECK(relay() == 0);
}

static void t_fh_relay_off_before_nvs(void)
{
    // The latch drops the relay before any NVS write of the result.
    fresh_boot();
    start(25.0f, 25.0f, 50.0f);
    env->nvs_set_hook = relay_low_at_result_write;
    run_window(0.0f, 0.0f, 100);
    env->nvs_set_hook = NULL;
    CHECK(pb_heater_fault_code() == PB_FAULT_HEATER_NO_RISE);
    CHECK(nvs_u("fh_state") == PB_FH_STORED_NO_RISE);
}

static void rerun_from_http(void)
{
    host_ctx_t saved = env->ctx;
    env->ctx = CTX_OTHER;
    CHECK(pb_heater_first_heat_rerun() == ESP_OK);
    env->ctx = saved;
}

static void t_fh_rerun_race(void)
{
    // A re-run lands in the tick that decides PASS: the PASS must not reach NVS.
    fresh_boot();
    start(25.0f, 25.0f, 50.0f);
    for (int i = 0; i < 23; ++i) step(0.1f, 1.0f);
    CHECK(fh().state == PB_FH_ARMED);
    env->ntc_read_hook = rerun_from_http;
    step(0.1f, 1.0f);                           // decides PASS
    CHECK(nvs_u("fh_state") == PB_FH_STORED_UNKNOWN);
    CHECK(fh().stored == PB_FH_STORED_UNKNOWN);
    step(0.0f, 0.0f);                           // the re-run applies
    CHECK(fh().state == PB_FH_IDLE);
    CHECK(nvs_u("fh_state") == PB_FH_STORED_UNKNOWN);
    advance_ms(5000);
    tick();
    CHECK(nvs_u("fh_state") == PB_FH_STORED_UNKNOWN);
}

// --- the hazard trips stay authoritative --------------------------------------------

static void t_fh_hazard_wins_no_rise(void)
{
    fresh_boot();
    start(25.0f, 25.0f, 50.0f);
    for (int i = 0; i < 59; ++i) step(0.0f, 0.0f);
    env->ntc_status[PB_NTC_PTC] = PB_NTC_OPEN;    // the tick where no-rise is due
    step(0.0f, 0.0f);
    CHECK(relay() == 0 && pb_heater_fault_code() == PB_FAULT_PTC_SENSOR);
    CHECK(fh().state == PB_FH_IDLE);
    CHECK(nvs_u("fault_code") == PB_FAULT_PTC_SENSOR);
    CHECK(nvs_u("fh_state") == -1);               // no result recorded
}

static void t_fh_hazard_wins_crossed(void)
{
    fresh_boot();
    start(25.0f, 25.0f, 70.0f);
    for (int i = 0; i < 23; ++i) step(0.5f, 0.0f);
    sensors(PB_HEATER_CHAMBER_MAX_C, 25.0f);     // the tick where WRONG_ROLES is due
    step(0.0f, 0.0f);
    CHECK(pb_heater_fault_code() == PB_FAULT_CHAMBER_OVERTEMP);
    CHECK(fh().state == PB_FH_IDLE && nvs_u("fh_state") == -1);
}

static void t_fh_hazard_wins_ptc_overtemp(void)
{
    fresh_boot();
    start(25.0f, 25.0f, 50.0f);
    for (int i = 0; i < 23; ++i) step(0.0f, 0.5f);
    sensors(30.0f, PB_HEATER_PTC_CUTOFF_C);
    step(0.0f, 0.0f);
    CHECK(pb_heater_fault_code() == PB_FAULT_PTC_OVERTEMP);
    CHECK(fh().state == PB_FH_IDLE);
}

static void t_fh_comms_wins(void)
{
    fresh_boot();
    CHECK(pb_heater_set_comms_timeout_ms(PB_HEATER_COMMS_TIMEOUT_MS_MIN) == ESP_OK);
    start(25.0f, 25.0f, 50.0f);
    // No link refresh: the watchdog trips at 10 s, before any decision.
    for (int i = 0; i < 30 && !pb_heater_is_faulted(); ++i) {
        advance_ms(500);
        tick();
    }
    CHECK(pb_heater_fault_code() == PB_FAULT_LINK_LOST);
    CHECK(fh().state == PB_FH_IDLE);
}

// --- constants, pure helpers ---------------------------------------------------------

static void t_fh_limits_unchanged(void)
{
    // The first-heat check adds no setting and changes no limit.
    CHECK(PB_HEATER_PTC_CUTOFF_C == 105.0f);
    CHECK(PB_HEATER_CHAMBER_MAX_C == 85.0f);
    CHECK(PB_HEATER_HYSTERESIS_C == 1.0f);
    CHECK(PB_HEATER_LOCAL_FOLDBACK_CUT_C == 72.0f && PB_HEATER_LOCAL_FOLDBACK_RESUME_C == 67.0f);
    CHECK(PB_HEATER_PTC_FOLDBACK_CUT_82K_C == 102.0f && PB_HEATER_PTC_FOLDBACK_RESUME_82K_C == 99.0f);
    CHECK(PB_HEATER_PTC_FOLDBACK_CUT_33K_C == 99.0f && PB_HEATER_PTC_FOLDBACK_RESUME_33K_C == 96.0f);
    CHECK(PB_HEATER_PTC_FOLDBACK_BAND_C == 3.0f);
    CHECK(PB_HEATER_FB_CUT_MIN_C == 90.0f && PB_HEATER_FB_CUT_MAX_C == 104.0f);
    CHECK(PB_HEATER_MAX_TARGET_C_DEFAULT == 70.0f && PB_HEATER_ABS_MAX_TARGET_C == 70.0f);
    CHECK(PB_HEATER_MIN_TARGET_C == 30.0f);
    CHECK(PB_HEATER_COMMS_TIMEOUT_MS_DEFAULT == 300000 && PB_HEATER_COMMS_TIMEOUT_MS_MIN == 10000 &&
          PB_HEATER_COMMS_TIMEOUT_MS_MAX == 300000);
    CHECK(PB_HEATER_COOL_RELEASE_C_DEFAULT == 40.0f && PB_HEATER_COOL_RELEASE_MIN_C == 30.0f &&
          PB_HEATER_COOL_RELEASE_MAX_C == 65.0f);
    // The persisted fault codes keep their values. The new ones are appended.
    CHECK(PB_FAULT_NVS_UNREADABLE == 9 && PB_FAULT_HEATER_NO_RISE == 10 &&
          PB_FAULT_SENSOR_ROLES == 11 && PB_FAULT__COUNT == 12);
    // The provisional first-heat thresholds (docs/SAFETY.md).
    CHECK(PB_FH_MIN_ON_MS == 12000 && PB_FH_MAX_ON_MS == 30000 && PB_FH_FAIL_WALL_MS == 60000 &&
          PB_FH_MAX_WALL_MS == 300000);
    CHECK(PB_FH_PASS_RISE_DC == 30 && PB_FH_NO_RISE_DC == 20 && PB_FH_ROLES_MARGIN_DC == 20 &&
          PB_FH_ROLES_EARLY_DC == 100 && PB_FH_HOT_START_DC == 700 && PB_FH_WARM_START_DC == 100);

    // A first-heat pass does not change a setting.
    fresh_boot();
    start(25.0f, 25.0f, 50.0f);
    run_window(0.1f, 1.0f, 100);
    CHECK(fh().state == PB_FH_PASS);
    CHECK(pb_heater_get_max_target_c() == PB_HEATER_MAX_TARGET_C_DEFAULT);
    CHECK(pb_heater_get_comms_timeout_ms() == PB_HEATER_COMMS_TIMEOUT_MS_DEFAULT);
    CHECK(pb_heater_get_fb_cut_c() == 0.0f);
    CHECK(pb_heater_get_cool_release_c() == PB_HEATER_COOL_RELEASE_C_DEFAULT);
    CHECK(pb_heater_get_method() == PB_HEATER_METHOD_BANGBANG);
    CHECK(pb_heater_get_target_c() == 50.0f);
}

static void t_fh_pure(void)
{
#define EV(on, p, c) pb_heater_first_heat_eval((on), (on), (p), (c))   // continuous heat
    CHECK(EV(11999, 500, 0) == PB_FH_EVAL_WAIT);
    CHECK(EV(11999, 0, 500) == PB_FH_EVAL_WAIT);
    CHECK(EV(12000, 30, 14) == PB_FH_EVAL_PASS);
    CHECK(EV(12000, 30, 15) == PB_FH_EVAL_WAIT);           // not > 2 x
    CHECK(EV(12000, 29, 0) == PB_FH_EVAL_WAIT);
    CHECK(EV(12000, 40, 100) == PB_FH_EVAL_WRONG_ROLES);   // early: 100 >= 2 x 40 + 20
    CHECK(EV(12000, 41, 100) == PB_FH_EVAL_WAIT);
    CHECK(EV(12000, 0, 99) == PB_FH_EVAL_WAIT);            // early needs 10 C
    CHECK(EV(29999, 0, 99) == PB_FH_EVAL_WAIT);
    CHECK(EV(30000, 0, 20) == PB_FH_EVAL_WRONG_ROLES);     // 20 >= 2 x 0 + 20
    CHECK(EV(30000, 0, 19) == PB_FH_EVAL_NO_RISE);
    CHECK(EV(30000, 19, 58) == PB_FH_EVAL_WRONG_ROLES);
    CHECK(EV(30000, 19, 57) == PB_FH_EVAL_NO_RISE);
    CHECK(EV(30000, 20, 59) == PB_FH_EVAL_UNCERTAIN);      // PTC rose 2 C, not 2 x + 2
    CHECK(EV(30000, 200, 400) == PB_FH_EVAL_UNCERTAIN);    // similar rises: never WRONG_ROLES
    CHECK(EV(30000, 200, 420) == PB_FH_EVAL_WRONG_ROLES);
    CHECK(EV(30000, 40, 25) == PB_FH_EVAL_UNCERTAIN);
    CHECK(pb_heater_first_heat_eval(UINT32_MAX, 30000, -5, -5) == PB_FH_EVAL_NO_RISE);
#undef EV
    // A low duty (wall time > 60 s) gives no failed result, but can pass.
    CHECK(pb_heater_first_heat_eval(30000, 60000, 0, 0) == PB_FH_EVAL_NO_RISE);
    CHECK(pb_heater_first_heat_eval(30000, 60001, 0, 0) == PB_FH_EVAL_WAIT);
    CHECK(pb_heater_first_heat_eval(12000, 60001, 0, 500) == PB_FH_EVAL_WAIT);
    CHECK(pb_heater_first_heat_eval(30000, 60001, 200, 300) == PB_FH_EVAL_WAIT);
    CHECK(pb_heater_first_heat_eval(30000, 250000, 30, 0) == PB_FH_EVAL_PASS);
    CHECK(pb_heater_first_heat_warm(700, 690) && !pb_heater_first_heat_warm(699, 690));
    CHECK(pb_heater_first_heat_warm(400, 300) && !pb_heater_first_heat_warm(399, 300));
    CHECK(!pb_heater_first_heat_warm(100, 300));          // crossed leads: PTC below chamber
    CHECK(pb_heater_median3(1, 2, 3) == 2 && pb_heater_median3(3, 1, 2) == 2 &&
          pb_heater_median3(2, 3, 1) == 2 && pb_heater_median3(-50, 250, 250) == 250 &&
          pb_heater_median3(250, 250, 900) == 250 && pb_heater_median3(5, 5, 5) == 5);
    CHECK(pb_heater_c_to_dc(25.04f) == 250 && pb_heater_c_to_dc(25.05f) == 251);
    CHECK(pb_heater_c_to_dc(-1.06f) == -11 && pb_heater_c_to_dc(0.0f) == 0);
    CHECK(pb_heater_c_to_dc(NAN) == -1000 && pb_heater_c_to_dc(-INFINITY) == -1000);
    CHECK(pb_heater_c_to_dc(INFINITY) == 3000 && pb_heater_c_to_dc(1e30f) == 3000);
    CHECK(strcmp(pb_heater_first_heat_state_str(PB_FH_NO_RISE), "no_rise") == 0);
    CHECK(strcmp(pb_heater_first_heat_state_str(PB_FH_UNCERTAIN), "uncertain") == 0);
    CHECK(strcmp(pb_heater_first_heat_state_str((pb_fh_state_t)99), "unknown") == 0);
    CHECK(strcmp(pb_heater_first_heat_stored_str(PB_FH_STORED_WRONG_ROLES), "wrong_roles") == 0);
    CHECK(strcmp(pb_heater_first_heat_state_str(PB_FH_WRONG_ROLES), "wrong_roles") == 0);
    CHECK(strcmp(pb_heater_first_heat_stock_str(PB_FH_STOCK_NORMAL), "normal") == 0);
    CHECK(strcmp(pb_heater_first_heat_stock_str((pb_fh_stock_hint_t)99), "invalid") == 0);
    CHECK(strcmp(pb_heater_fault_str(PB_FAULT_HEATER_NO_RISE), "heater no-rise (first-heat check)") == 0);
    pb_heater_get_first_heat(NULL);
}

// --- NVS failures ------------------------------------------------------------------

static void t_fh_nvs_fail_pass(void)
{
    fresh_boot();
    env->nvs_fail_set = ESP_FAIL;
    start(25.0f, 25.0f, 50.0f);
    run_window(0.1f, 1.0f, 100);
    CHECK(fh().state == PB_FH_PASS && relay() == 1 && !pb_heater_is_faulted());
    CHECK(nvs_u("fh_state") == -1);
    for (int i = 0; i < 20; ++i) step(0.0f, 0.0f);         // retries fail, no crash
    CHECK(nvs_u("fh_state") == -1);
    env->nvs_fail_set = 0;
    for (int i = 0; i < 5; ++i) step(0.0f, 0.0f);          // within 2.5 s
    CHECK(nvs_u("fh_state") == PB_FH_STORED_PASS && nvs_u("fh_ptc_dc") == 220);
    unsigned sets = env->nvs_sets;
    for (int i = 0; i < 20; ++i) step(0.0f, 0.0f);
    CHECK(env->nvs_sets == sets);                           // nothing pending any more
}

static void t_fh_nvs_fail_no_rise(void)
{
    // Fail closed: the latch holds in RAM, both records land after recovery.
    fresh_boot();
    env->nvs_fail_open_rw = ESP_ERR_NVS_NOT_INITIALIZED;
    start(25.0f, 25.0f, 50.0f);
    run_window(0.0f, 0.0f, 100);
    CHECK(relay() == 0 && pb_heater_fault_code() == PB_FAULT_HEATER_NO_RISE);
    CHECK(nvs_u("fault_latch") == -1 && nvs_u("fh_state") == -1);
    for (int i = 0; i < 10; ++i) { step(0.0f, 0.0f); CHECK(relay() == 0); }
    env->nvs_fail_open_rw = 0;
    for (int i = 0; i < 5; ++i) step(0.0f, 0.0f);
    CHECK(nvs_u("fault_latch") == 1 && nvs_u("fault_code") == PB_FAULT_HEATER_NO_RISE);
    CHECK(nvs_u("fh_state") == PB_FH_STORED_NO_RISE);
}

static void t_fh_nvs_partial_never_old_pass(void)
{
    // Flash holds a pass that the boot could not read, so the check runs. Its
    // no-rise result fails part way. Flash must not keep the old pass.
    env_nvs_write("fh_state", HOST_NVS_U8, PB_FH_STORED_PASS);
    env_nvs_write("fh_cfg", HOST_NVS_U32, fh_cfg_expected());
    env->nvs_fail_get = ESP_FAIL;
    fresh_boot();
    env->nvs_fail_get = 0;
    CHECK(pb_heater_fault_code() == PB_FAULT_NVS_UNREADABLE);
    CHECK(clear_fault() == ESP_OK);
    CHECK(fh().stored == PB_FH_STORED_UNKNOWN);
    env->nvs_fail_set_nth = 3;                   // the 3rd set of each handle fails
    start(25.0f, 25.0f, 50.0f);
    CHECK(fh().state == PB_FH_ARMED);
    run_window(0.0f, 0.0f, 100);
    CHECK(pb_heater_fault_code() == PB_FAULT_HEATER_NO_RISE);
    CHECK(nvs_u("fh_state") == PB_FH_STORED_UNKNOWN);
}

static void t_fh_nvs_partial_phase2(void)
{
    env->nvs_fail_set_nth = 0;
    boot();
    CHECK(pb_heater_fault_code() == PB_FAULT_HEATER_NO_RISE);   // the latch landed
    CHECK(fh().stored != PB_FH_STORED_PASS);
}

static void t_fh_read_error_runs_check(void)
{
    // An unreadable stored result runs the check again (fail-closed).
    env_nvs_write("fh_state", HOST_NVS_U8, PB_FH_STORED_PASS);
    env_nvs_write("fh_cfg", HOST_NVS_U32, fh_cfg_expected());
    env->nvs_fail_get = ESP_FAIL;
    boot();
    CHECK(pb_heater_fault_code() == PB_FAULT_NVS_UNREADABLE);
    CHECK(fh().stored == PB_FH_STORED_UNKNOWN && fh().state == PB_FH_IDLE);
    CHECK(fh().stock_hint == PB_FH_STOCK_INVALID);
}

static void t_fh_corrupt_record(void)
{
    // An out-of-range state or a missing fingerprint is not a pass.
    env_nvs_write("fh_state", HOST_NVS_U8, 7);
    env_nvs_write("fh_cfg", HOST_NVS_U32, fh_cfg_expected());
    boot();
    CHECK(fh().stored == PB_FH_STORED_UNKNOWN);
    env_nvs_write("fh_state", HOST_NVS_U8, PB_FH_STORED_PASS);
    env_nvs_erase("fh_cfg");
    boot();
    CHECK(fh().stored == PB_FH_STORED_UNKNOWN && fh().state == PB_FH_IDLE);
    CHECK(nvs_u("fh_state") == PB_FH_STORED_UNKNOWN);       // the stale pass is reset
    env_nvs_write("fh_state", HOST_NVS_U32, PB_FH_STORED_PASS);   // wrong type
    env_nvs_write("fh_cfg", HOST_NVS_U32, fh_cfg_expected());
    boot();
    CHECK(fh().stored == PB_FH_STORED_UNKNOWN);
}

// --- re-run --------------------------------------------------------------------------

static void t_fh_rerun(void)
{
    boot();                                      // seeded pass
    CHECK(fh().state == PB_FH_PASS);
    env->ctx = CTX_OTHER;
    CHECK(pb_heater_first_heat_rerun() == ESP_OK);
    env_check_quiescent();
    CHECK(nvs_u("fh_state") == PB_FH_STORED_UNKNOWN);
    CHECK(fh().stored == PB_FH_STORED_UNKNOWN);
    CHECK(fh().state == PB_FH_PASS);             // the control task applies it
    tick();
    CHECK(fh().state == PB_FH_IDLE);
    start(25.0f, 25.0f, 50.0f);
    CHECK(fh().state == PB_FH_ARMED);
    // A re-run while a window is open closes it. The next relay OFF->ON edge
    // opens a new one.
    for (int i = 0; i < 20; ++i) step(0.0f, 0.0f);
    CHECK(pb_heater_first_heat_rerun() == ESP_OK);
    step(0.0f, 0.0f);
    CHECK(fh().state == PB_FH_IDLE && relay() == 1);
    CHECK(set_target(0.0f) == ESP_OK);
    step(0.0f, 0.0f);
    CHECK(set_target(50.0f) == ESP_OK);
    step(0.0f, 0.0f);
    CHECK(fh().state == PB_FH_ARMED && fh().on_ms == 0);
}

static void t_fh_rerun_nvs_fails(void)
{
    boot();
    env->nvs_fail_open_rw = ESP_FAIL;
    env->ctx = CTX_OTHER;
    CHECK(pb_heater_first_heat_rerun() == ESP_FAIL);
    env_check_quiescent();
    tick();
    CHECK(fh().state == PB_FH_IDLE);            // the RAM re-run applies anyway
    CHECK(nvs_u("fh_state") == PB_FH_STORED_PASS);
    env->nvs_fail_open_rw = 0;
    advance_ms(2500);
    tick();
    CHECK(nvs_u("fh_state") == PB_FH_STORED_UNKNOWN);
}

static void t_fh_rerun_keeps_latch(void)
{
    // A re-run does not clear a first-heat fault. Only the clear does.
    fresh_boot();
    start(25.0f, 25.0f, 50.0f);
    run_window(0.0f, 0.0f, 100);
    CHECK(pb_heater_first_heat_rerun() == ESP_OK);
    tick();
    CHECK(pb_heater_is_faulted() && fh().state == PB_FH_NO_RISE);
    CHECK(fh().stored == PB_FH_STORED_UNKNOWN);
}

// --- stock hint ------------------------------------------------------------------------

static pb_fh_stock_hint_t hint_after_boot(void)
{
    boot();
    CHECK(fh().stored == PB_FH_STORED_UNKNOWN && fh().state == PB_FH_IDLE);
    CHECK(!env->ntc_swap_active);                // a hint only, never applied
    pb_fh_stock_hint_t h = fh().stock_hint;
    env_nvs_erase("ntc_bit");
    return h;
}

static void t_fh_stock_hint(void)
{
    g_fh_seed_pass = false;
    CHECK(hint_after_boot() == PB_FH_STOCK_NONE);            // fresh device
    env_nvs_write("heat_max_c", HOST_NVS_U32, 7000);
    CHECK(hint_after_boot() == PB_FH_STOCK_NONE);            // no key
    env_nvs_write_blob("ntc_bit", 1, 4);                     // stock layout
    CHECK(hint_after_boot() == PB_FH_STOCK_CROSSED);
    env_nvs_write_blob("ntc_bit", 2, 4);
    CHECK(hint_after_boot() == PB_FH_STOCK_NORMAL);
    env_nvs_write_blob("ntc_bit", 0, 4);
    CHECK(hint_after_boot() == PB_FH_STOCK_UNKNOWN);
    env_nvs_write_blob("ntc_bit", 0x101, 4);
    CHECK(hint_after_boot() == PB_FH_STOCK_INVALID);
    env_nvs_write_blob("ntc_bit", 2, 1);
    CHECK(hint_after_boot() == PB_FH_STOCK_NORMAL);
    env_nvs_write_blob("ntc_bit", 1, 8);                     // too large
    CHECK(hint_after_boot() == PB_FH_STOCK_INVALID);
    env_nvs_write_blob("ntc_bit", 0, 0);                     // empty
    CHECK(hint_after_boot() == PB_FH_STOCK_INVALID);
    env_nvs_write("ntc_bit", HOST_NVS_U8, 1);                // other types
    CHECK(hint_after_boot() == PB_FH_STOCK_CROSSED);
    env_nvs_write("ntc_bit", HOST_NVS_I32, 2);
    CHECK(hint_after_boot() == PB_FH_STOCK_NORMAL);
    env_nvs_write("ntc_bit", HOST_NVS_U32, 3);
    CHECK(hint_after_boot() == PB_FH_STOCK_INVALID);
    env_nvs_write_blob("ntc_bit", 1, 4);
    env->nvs_fail_open_ro = ESP_FAIL;
    boot();
    CHECK(fh().stock_hint == PB_FH_STOCK_INVALID);
}

static void t_fh_stock_hint_crossed_still_checks(void)
{
    // The stock hint does not replace the check: a normal plant passes.
    fresh_boot();
    env_nvs_write_blob("ntc_bit", 1, 4);
    boot();
    start(25.0f, 25.0f, 50.0f);
    run_window(0.1f, 1.0f, 100);
    CHECK(fh().state == PB_FH_PASS && fh().stock_hint == PB_FH_STOCK_CROSSED);
}

bool heater_first_heat_tests(void)
{
    bool ok = true;
#define RUN(fn)      do { env_reset(); ok &= env_run(#fn, fn); } while (0)
#define RUN_KEEP(fn) do { ok &= env_run(#fn, fn); } while (0)
    RUN(t_fh_pass_phase1);
    RUN_KEEP(t_fh_pass_phase2_skips);
    RUN_KEEP(t_fh_pass_phase3_offset_resets);
    RUN(t_fh_pass_pid);
    RUN(t_fh_crossed_phase1);
    RUN_KEEP(t_fh_crossed_phase2_swap_applied);
    RUN(t_fh_crossed_restored);
    RUN(t_fh_dead_phase1);
    RUN_KEEP(t_fh_dead_phase2_still_latched);
    RUN(t_fh_detached_ptc);
    RUN(t_fh_both_rise_uncertain);
    RUN(t_fh_small_ptc_lead_uncertain);
    RUN(t_fh_roles_margins);
    RUN(t_fh_glitch);
    RUN(t_fh_relay_off_not_counted);
    RUN(t_fh_low_duty_never_fails);
    RUN(t_fh_wall_timeout);
    RUN(t_fh_stop_aborts);
    RUN(t_fh_panic_aborts);
    RUN(t_fh_clear_and_target_before_tick);
    RUN(t_fh_hot_start);
    RUN(t_fh_start_glitch);
    RUN(t_fh_offset_change_restarts);
    RUN(t_fh_relay_off_before_nvs);
    RUN(t_fh_rerun_race);
    RUN(t_fh_hazard_wins_no_rise);
    RUN(t_fh_hazard_wins_crossed);
    RUN(t_fh_hazard_wins_ptc_overtemp);
    RUN(t_fh_comms_wins);
    RUN(t_fh_limits_unchanged);
    RUN(t_fh_pure);
    RUN(t_fh_nvs_fail_pass);
    RUN(t_fh_nvs_fail_no_rise);
    RUN(t_fh_nvs_partial_never_old_pass);
    RUN_KEEP(t_fh_nvs_partial_phase2);
    RUN(t_fh_read_error_runs_check);
    RUN(t_fh_corrupt_record);
    RUN(t_fh_rerun);
    RUN(t_fh_rerun_nvs_fails);
    RUN(t_fh_rerun_keeps_latch);
    RUN(t_fh_stock_hint);
    RUN(t_fh_stock_hint_crossed_still_checks);
#undef RUN
#undef RUN_KEEP
    return ok;
}
