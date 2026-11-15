// SPDX-License-Identifier: MIT
// Host test for the REAL pb_heater.c control tick: the code that writes the
// relay GPIO. The pure decision helpers have their own tests. This test links
// pb_heater.c itself with stubs (tests/stubs_heater/, tests/heater_host_env.c)
// for the GPIO, the clock, NVS, FreeRTOS and the pb_ntc sensor API.
//
// Part 1: directed tests for each safety path in docs/SAFETY.md.
// Part 2: directed tests of the first-heat check (pb_heater_first_heat_cases.c).
// Part 3: a randomized test with fixed seeds that checks invariants after each
//         tick (see pb_heater_tick_random.c).
//
// Each scenario runs in a forked child, so that the static state of
// pb_heater.c starts as on a fresh boot. NVS survives a "reboot" (a new child).
#define _DEFAULT_SOURCE
#include "pb_heater.h"
#include "pb_heater_tick_host_test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- pb_ntc API stub ------------------------------------------------------------
// pb_heater.c calls only these two. The contract of the real pb_ntc_read(): on a
// status other than OK the temperature is NAN.
pb_ntc_status_t pb_ntc_read(pb_ntc_channel_t ch, float *out_c)
{
    CHECK(ch == PB_NTC_CHAMBER || ch == PB_NTC_PTC);
    env->ntc_reads++;
    if (ch == PB_NTC_PTC && env->ntc_read_hook) {
        void (*hook)(void) = env->ntc_read_hook;   // another task runs at this point
        env->ntc_read_hook = NULL;
        hook();
    }
    pb_ntc_status_t st = (pb_ntc_status_t)env->ntc_status[ch];
    if (out_c) *out_c = (st == PB_NTC_OK) ? env->ntc_temp[ch] : NAN;
    return st;
}

int pb_ntc_rref_kohm(void) { return env->rref_kohm; }
bool pb_ntc_get_swap_thermistors_active(void) { return env->ntc_swap_active; }
float pb_ntc_get_offset_c(pb_ntc_channel_t ch) { return env->ntc_offset_c[ch]; }

// --- helpers ---------------------------------------------------------------------

int relay(void) { return env->level[PB_GPIO_RELAY]; }

void tick(void)
{
    env->ctx = CTX_CONTROL;
    pb_heater_tick();
    env->ctx = CTX_OTHER;
    env_check_quiescent();
    CHECK(pb_heater_is_on() == (relay() == 1));
}

bool g_fh_seed_pass = true;

uint32_t fh_cfg_expected(void)
{
    // The fingerprint of pb_heater.c: swap flag, then both offsets in centi-C.
    int32_t c = (int32_t)lroundf(env->ntc_offset_c[PB_NTC_CHAMBER] * 100.0f);
    int32_t p = (int32_t)lroundf(env->ntc_offset_c[PB_NTC_PTC] * 100.0f);
    return 0x80000000u | (env->ntc_swap_active ? 1u : 0u) |
           ((uint32_t)(c + 512) << 1) | ((uint32_t)(p + 512) << 11);
}

void boot(void)
{
    // The tests of the other safety paths heat a plant without a rise. They
    // start from a stored first-heat pass (the check ran on an earlier boot).
    // The first-heat tests and the random test set g_fh_seed_pass to false.
    if (g_fh_seed_pass && nvs_u("fh_state") < 0) {
        env_nvs_write("fh_state", HOST_NVS_U8, 1);
        env_nvs_write("fh_cfg", HOST_NVS_U32, fh_cfg_expected());
    }
    env->ctx = CTX_BOOT;
    CHECK(pb_heater_init() == ESP_OK);
    CHECK(relay() == 0);
    pb_heater_load_config();
    pb_heater_load_fault();
    pb_heater_load_first_heat();
    env->ctx = CTX_OTHER;
    env_check_quiescent();
    CHECK(relay() == 0);
    CHECK(pb_heater_get_target_c() == 0.0f);
}

void advance_ms(int64_t ms) { env->now_us += ms * 1000; }

void sensors(float chamber_c, float ptc_c)
{
    env->ntc_status[PB_NTC_CHAMBER] = PB_NTC_OK;
    env->ntc_status[PB_NTC_PTC] = PB_NTC_OK;
    env->ntc_temp[PB_NTC_CHAMBER] = chamber_c;
    env->ntc_temp[PB_NTC_PTC] = ptc_c;
}

esp_err_t set_target(float c)
{
    env->ctx = CTX_OTHER;
    esp_err_t r = pb_heater_set_target_c(c);
    env_check_quiescent();
    return r;
}

void link_alive(void)
{
    env->ctx = CTX_OTHER;
    pb_heater_notify_link_alive();
    env_check_quiescent();
}

esp_err_t clear_fault(void)
{
    env->ctx = CTX_OTHER;
    esp_err_t r = pb_heater_clear_fault();
    env_check_quiescent();
    return r;
}

int nvs_u(const char *key)
{
    uint32_t v;
    return env_nvs_read(key, &v) ? (int)v : -1;
}

// Boot, then heat from a cold chamber: the relay must be on.
static void start_heating(float target)
{
    boot();
    sensors(30.0f, 60.0f);
    link_alive();
    CHECK(set_target(target) == ESP_OK);
    tick();
    CHECK(relay() == 1);
}

static void expect_trip(pb_fault_reason_t code, bool persisted)
{
    tick();
    CHECK(relay() == 0);
    CHECK(pb_heater_is_faulted());
    CHECK(pb_heater_fault_code() == code);
    CHECK(pb_heater_get_target_c() == 0.0f);
    CHECK(!pb_heater_heat_mode());
    if (persisted) {
        CHECK(nvs_u("fault_latch") == 1);
        CHECK(nvs_u("fault_code") == (int)code);
    }
}

// --- directed tests ------------------------------------------------------------------

static void t_boot_relay_off(void)
{
    env->level[PB_GPIO_RELAY] = 1;          // pin state before init is undefined
    env->ctx = CTX_BOOT;
    CHECK(pb_heater_init() == ESP_OK);
    CHECK(env->gpio_config_calls == 1);
    CHECK(env->gpio_config_mask == (1ULL << PB_GPIO_RELAY));
    CHECK(env->gpio_config_mode == GPIO_MODE_OUTPUT);
    CHECK(env->writes[PB_GPIO_RELAY] >= 1 && env->high_writes[PB_GPIO_RELAY] == 0);
    CHECK(relay() == 0);
    CHECK(!pb_heater_is_on());
    CHECK(!pb_heater_is_faulted());
    CHECK(!pb_heater_heat_mode());
    CHECK(pb_heater_get_target_c() == 0.0f);
    CHECK(pb_heater_get_max_target_c() == PB_HEATER_MAX_TARGET_C_DEFAULT);
    CHECK(pb_heater_get_comms_timeout_ms() == PB_HEATER_COMMS_TIMEOUT_MS_DEFAULT);
    CHECK(pb_heater_get_method() == PB_HEATER_METHOD_BANGBANG);
    pb_heater_telemetry_t tel;
    pb_heater_get_telemetry(&tel);
    pb_heater_get_telemetry(NULL);
    CHECK(tel.constraint == PB_HEATER_CONSTRAINT_OFF && tel.commanded_duty == 0.0f);
    env_check_quiescent();

    // Idle ticks with cold sensors never energize the relay.
    sensors(20.0f, 20.0f);
    for (int i = 0; i < 10; ++i) { advance_ms(500); tick(); CHECK(relay() == 0); }
    // Init is idempotent and forces the relay off again.
    env->ctx = CTX_BOOT;
    CHECK(pb_heater_init() == ESP_OK);
    CHECK(relay() == 0);
}

static void t_boot_gpio_config_fails(void)
{
    env->gpio_config_err = ESP_FAIL;
    env->ctx = CTX_BOOT;
    // app_main wraps init in ESP_ERROR_CHECK, so an error aborts the boot.
    CHECK(pb_heater_init() == ESP_FAIL);
    CHECK(!pb_heater_is_on());
}

static void t_basic_bangbang(void)
{
    start_heating(50.0f);
    CHECK(pb_heater_heat_mode());
    pb_heater_telemetry_t tel;
    pb_heater_get_telemetry(&tel);
    CHECK(tel.constraint == PB_HEATER_CONSTRAINT_NONE && tel.commanded_duty == 1.0f);
    // Hysteresis: off at target, on again below target - 1 C.
    sensors(50.0f, 80.0f); tick(); CHECK(relay() == 0);
    pb_heater_get_telemetry(&tel);
    CHECK(tel.constraint == PB_HEATER_CONSTRAINT_TARGET_REACHED);
    sensors(49.5f, 80.0f); tick(); CHECK(relay() == 0);
    sensors(48.9f, 80.0f); tick(); CHECK(relay() == 1);
    // Target 0 turns the relay off and is not a fault.
    CHECK(set_target(0.0f) == ESP_OK);
    tick();
    CHECK(relay() == 0 && !pb_heater_is_faulted());
    CHECK(pb_heater_fault_reason() == NULL);
}

static void t_ptc_overtemp_latches(void)
{
    start_heating(60.0f);
    sensors(40.0f, 104.9f); tick();
    CHECK(!pb_heater_is_faulted());          // foldback, not a trip
    CHECK(relay() == 0);
    sensors(40.0f, PB_HEATER_PTC_CUTOFF_C);
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);
    CHECK(strcmp(pb_heater_fault_reason(), "PTC element over-temp") == 0);
    // It latches: cool sensors do not bring heat back.
    sensors(30.0f, 40.0f);
    for (int i = 0; i < 20; ++i) { advance_ms(500); link_alive(); tick(); CHECK(relay() == 0); }
    CHECK(pb_heater_is_faulted());
    CHECK(set_target(50.0f) == ESP_ERR_INVALID_STATE);
    CHECK(pb_heater_get_target_c() == 0.0f);
    CHECK(set_target(0.0f) == ESP_OK);       // a zero target is always accepted
}

static void t_ptc_overtemp_trips_when_idle(void)
{
    boot();
    sensors(25.0f, 150.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);
    // PTC over-temp has priority over chamber over-temp.
    CHECK(clear_fault() == ESP_OK);
    sensors(90.0f, 106.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);
}

static void t_foldback_82k(void)
{
    env->rref_kohm = 82;
    start_heating(70.0f);
    pb_heater_telemetry_t tel;
    sensors(40.0f, 101.9f); tick(); CHECK(relay() == 1);
    sensors(40.0f, 102.0f); tick(); CHECK(relay() == 0);
    pb_heater_get_telemetry(&tel);
    CHECK(tel.constraint == PB_HEATER_CONSTRAINT_ELEMENT_FOLDBACK);
    CHECK(!pb_heater_is_faulted());
    // Hold off inside the band [99, 102).
    sensors(40.0f, 101.0f); tick(); CHECK(relay() == 0);
    sensors(40.0f, 99.0f);  tick(); CHECK(relay() == 0);
    // Resume below 99.
    sensors(40.0f, 98.9f);  tick(); CHECK(relay() == 1);
    sensors(40.0f, 100.0f); tick(); CHECK(relay() == 1);   // band, previous state = on
    CHECK(!pb_heater_is_faulted());
}

static void t_foldback_33k(void)
{
    env->rref_kohm = 33;
    start_heating(70.0f);
    sensors(40.0f, 98.9f); tick(); CHECK(relay() == 1);
    sensors(40.0f, 99.0f); tick(); CHECK(relay() == 0);
    sensors(40.0f, 96.0f); tick(); CHECK(relay() == 0);
    sensors(40.0f, 95.9f); tick(); CHECK(relay() == 1);
}

static void t_foldback_user_override(void)
{
    start_heating(70.0f);
    env->ctx = CTX_OTHER;
    CHECK(pb_heater_set_fb_cut_c(95.0f) == ESP_OK);
    CHECK(pb_heater_get_fb_cut_c() == 95.0f);
    CHECK(nvs_u("fb_cut_c") == 9500);
    sensors(40.0f, 95.0f); tick(); CHECK(relay() == 0);
    sensors(40.0f, 92.0f); tick(); CHECK(relay() == 0);
    sensors(40.0f, 91.9f); tick(); CHECK(relay() == 1);
    // Out-of-band values clamp to [90, 104]. The override can never reach 105.
    CHECK(pb_heater_set_fb_cut_c(200.0f) == ESP_OK);
    CHECK(pb_heater_get_fb_cut_c() == PB_HEATER_FB_CUT_MAX_C);
    sensors(40.0f, 103.9f); tick(); CHECK(relay() == 1);
    sensors(40.0f, 104.0f); tick(); CHECK(relay() == 0);
    CHECK(pb_heater_set_fb_cut_c(10.0f) == ESP_OK);
    CHECK(pb_heater_get_fb_cut_c() == PB_HEATER_FB_CUT_MIN_C);
    CHECK(pb_heater_set_fb_cut_c(NAN) == ESP_ERR_INVALID_ARG);
    CHECK(pb_heater_get_fb_cut_c() == PB_HEATER_FB_CUT_MIN_C);
    CHECK(pb_heater_set_fb_cut_c(-1.0f) == ESP_OK);           // back to auto
    CHECK(pb_heater_get_fb_cut_c() == 0.0f);
    CHECK(nvs_u("fb_cut_c") == 0);
    env_check_quiescent();
}

static void t_foldback_nvs_clamped(void)
{
    env_nvs_write("fb_cut_c", HOST_NVS_U32, 20000);   // 200 C, hand-edited
    boot();
    CHECK(pb_heater_get_fb_cut_c() == PB_HEATER_FB_CUT_MAX_C);
}

static void t_foldback_nvs_low_clamped(void)
{
    env_nvs_write("fb_cut_c", HOST_NVS_U32, 100);      // 1 C
    boot();
    CHECK(pb_heater_get_fb_cut_c() == PB_HEATER_FB_CUT_MIN_C);
}

static void t_chamber_overtemp(void)
{
    start_heating(70.0f);
    sensors(84.9f, 90.0f); tick();
    CHECK(!pb_heater_is_faulted() && relay() == 0);   // above target, not a trip
    sensors(PB_HEATER_CHAMBER_MAX_C, 90.0f);
    expect_trip(PB_FAULT_CHAMBER_OVERTEMP, true);
    CHECK(clear_fault() == ESP_OK);
    // Idle: still trips.
    sensors(99.0f, 40.0f);
    expect_trip(PB_FAULT_CHAMBER_OVERTEMP, true);
}

static void t_chamber_overtemp_external_regulation(void)
{
    // A remote chamber sensor for regulation must not hide the local cutoff.
    start_heating(70.0f);
    pb_heater_set_control_chamber_c(40.0f);
    tick(); CHECK(relay() == 1);
    sensors(85.0f, 60.0f);
    expect_trip(PB_FAULT_CHAMBER_OVERTEMP, true);
}

static void t_local_foldback_external(void)
{
    start_heating(70.0f);
    pb_heater_set_control_chamber_c(40.0f);
    pb_heater_telemetry_t tel;
    sensors(71.9f, 60.0f); tick(); CHECK(relay() == 1);
    sensors(72.0f, 60.0f); tick(); CHECK(relay() == 0);
    pb_heater_get_telemetry(&tel);
    CHECK(tel.constraint == PB_HEATER_CONSTRAINT_LOCAL_FOLDBACK);
    sensors(67.0f, 60.0f); tick(); CHECK(relay() == 0);
    sensors(66.9f, 60.0f); tick(); CHECK(relay() == 1);
    // Without the external value the local NTC regulates: 66.9 < 70 - 1 heats,
    // and the local soft limit is not active.
    pb_heater_set_control_chamber_c(NAN);
    sensors(72.0f, 60.0f); tick(); CHECK(relay() == 0);   // 72 >= 70: target reached
    pb_heater_set_control_chamber_c(INFINITY);            // non-finite -> local NTC
    sensors(60.0f, 60.0f); tick(); CHECK(relay() == 1);
}

static void sensor_fault_case(pb_ntc_channel_t ch, pb_ntc_status_t st)
{
    start_heating(60.0f);
    env->ntc_status[ch] = st;
    expect_trip(ch == PB_NTC_CHAMBER ? PB_FAULT_CHAMBER_SENSOR : PB_FAULT_PTC_SENSOR, true);
}

static void t_sensor_fault_chamber_open(void)  { sensor_fault_case(PB_NTC_CHAMBER, PB_NTC_OPEN); }
static void t_sensor_fault_chamber_short(void) { sensor_fault_case(PB_NTC_CHAMBER, PB_NTC_SHORT); }
static void t_sensor_fault_chamber_uninit(void){ sensor_fault_case(PB_NTC_CHAMBER, PB_NTC_UNINIT); }
static void t_sensor_fault_ptc_open(void)      { sensor_fault_case(PB_NTC_PTC, PB_NTC_OPEN); }
static void t_sensor_fault_ptc_short(void)     { sensor_fault_case(PB_NTC_PTC, PB_NTC_SHORT); }
static void t_sensor_fault_ptc_uninit(void)    { sensor_fault_case(PB_NTC_PTC, PB_NTC_UNINIT); }

static void t_sensor_fault_idle_then_arm(void)
{
    boot();
    sensors(30.0f, 30.0f);
    env->ntc_status[PB_NTC_PTC] = PB_NTC_OPEN;
    for (int i = 0; i < 5; ++i) { tick(); CHECK(!pb_heater_is_faulted()); }   // idle: no hazard
    link_alive();
    CHECK(set_target(50.0f) == ESP_OK);
    unsigned high = env->high_writes[PB_GPIO_RELAY];
    expect_trip(PB_FAULT_PTC_SENSOR, true);
    CHECK(env->high_writes[PB_GPIO_RELAY] == high);   // no relay-on write in that tick
}

static void t_comms_watchdog(void)
{
    boot();
    env->ctx = CTX_OTHER;
    CHECK(pb_heater_set_comms_timeout_ms(1) == ESP_OK);           // clamps to the minimum
    CHECK(pb_heater_get_comms_timeout_ms() == PB_HEATER_COMMS_TIMEOUT_MS_MIN);
    CHECK(nvs_u("heat_comms_ms") == PB_HEATER_COMMS_TIMEOUT_MS_MIN);
    sensors(30.0f, 60.0f);
    link_alive();
    CHECK(set_target(50.0f) == ESP_OK);
    tick(); CHECK(relay() == 1);
    advance_ms(PB_HEATER_COMMS_TIMEOUT_MS_MIN);           // exactly the timeout: not expired
    tick(); CHECK(relay() == 1 && !pb_heater_is_faulted());
    link_alive();
    advance_ms(PB_HEATER_COMMS_TIMEOUT_MS_MIN - 1);
    tick(); CHECK(relay() == 1);
    link_alive();                                          // refresh keeps it alive
    advance_ms(PB_HEATER_COMMS_TIMEOUT_MS_MIN);
    env->now_us += 1;
    expect_trip(PB_FAULT_LINK_LOST, true);
    CHECK(strcmp(pb_heater_fault_reason(), "controller link lost") == 0);
}

static void t_comms_watchdog_default_and_max(void)
{
    boot();
    env->ctx = CTX_OTHER;
    CHECK(pb_heater_set_comms_timeout_ms(3600u * 1000u) == ESP_OK);   // clamps to 5 min
    CHECK(pb_heater_get_comms_timeout_ms() == PB_HEATER_COMMS_TIMEOUT_MS_MAX);
    sensors(30.0f, 60.0f);
    link_alive();
    CHECK(set_target(50.0f) == ESP_OK);
    for (int i = 0; i < 600; ++i) { advance_ms(500); tick(); CHECK(!pb_heater_is_faulted()); }
    env->now_us += 1;
    expect_trip(PB_FAULT_LINK_LOST, true);
}

static void t_comms_idle_stale_link(void)
{
    boot();
    sensors(30.0f, 60.0f);
    advance_ms(PB_HEATER_COMMS_TIMEOUT_MS_DEFAULT + 1000);
    tick();
    CHECK(!pb_heater_is_faulted());          // idle: the watchdog does not apply
    // Arming without a link refresh trips on the first tick, before any heat.
    CHECK(set_target(50.0f) == ESP_OK);
    unsigned high = env->high_writes[PB_GPIO_RELAY];
    expect_trip(PB_FAULT_LINK_LOST, true);
    CHECK(env->high_writes[PB_GPIO_RELAY] == high);
}

static void t_comms_nvs_clamped(void)
{
    env_nvs_write("heat_comms_ms", HOST_NVS_U32, 0);
    boot();
    CHECK(pb_heater_get_comms_timeout_ms() == PB_HEATER_COMMS_TIMEOUT_MS_MIN);
}

static void t_comms_nvs_high_clamped(void)
{
    env_nvs_write("heat_comms_ms", HOST_NVS_U32, 0xFFFFFFFFu);
    boot();
    CHECK(pb_heater_get_comms_timeout_ms() == PB_HEATER_COMMS_TIMEOUT_MS_MAX);
}

static void t_target_clamp(void)
{
    boot();
    CHECK(set_target(100.0f) == ESP_OK);
    CHECK(pb_heater_get_target_c() == PB_HEATER_ABS_MAX_TARGET_C);
    CHECK(set_target(-5.0f) == ESP_OK);
    CHECK(pb_heater_get_target_c() == 0.0f);
    CHECK(set_target(NAN) == ESP_ERR_INVALID_ARG);
    CHECK(set_target(INFINITY) == ESP_ERR_INVALID_ARG);
    CHECK(pb_heater_get_target_c() == 0.0f);
    CHECK(set_target(65.0f) == ESP_OK);
    env->ctx = CTX_OTHER;
    CHECK(pb_heater_set_max_target_c(60.0f) == ESP_OK);
    CHECK(pb_heater_get_target_c() == 60.0f);              // live target pulled down
    CHECK(nvs_u("heat_max_c") == 6000);
    CHECK(pb_heater_set_max_target_c(500.0f) == ESP_OK);
    CHECK(pb_heater_get_max_target_c() == PB_HEATER_ABS_MAX_TARGET_C);
    CHECK(pb_heater_set_max_target_c(5.0f) == ESP_OK);
    CHECK(pb_heater_get_max_target_c() == PB_HEATER_MIN_TARGET_C);
    CHECK(pb_heater_get_target_c() == PB_HEATER_MIN_TARGET_C);
    CHECK(pb_heater_set_max_target_c(NAN) == ESP_ERR_INVALID_ARG);
    CHECK(pb_heater_get_max_target_c() == PB_HEATER_MIN_TARGET_C);
    CHECK(pb_heater_set_max_target_c(70.0f) == ESP_OK);
    // Behavior at the clamp: a 100 C request regulates to 70 C.
    sensors(68.9f, 80.0f);
    link_alive();
    CHECK(set_target(100.0f) == ESP_OK);
    tick(); CHECK(relay() == 1);
    sensors(70.0f, 80.0f); tick(); CHECK(relay() == 0);
    env_check_quiescent();
}

static void t_target_clamp_nvs(void)
{
    env_nvs_write("heat_max_c", HOST_NVS_U32, 20000);   // 200 C, hand-edited
    boot();
    CHECK(pb_heater_get_max_target_c() == PB_HEATER_ABS_MAX_TARGET_C);
    CHECK(set_target(80.0f) == ESP_OK);
    CHECK(pb_heater_get_target_c() == PB_HEATER_ABS_MAX_TARGET_C);
}

static void t_target_clamp_nvs_low(void)
{
    env_nvs_write("heat_max_c", HOST_NVS_U32, 0);
    boot();
    CHECK(pb_heater_get_max_target_c() == PB_HEATER_MIN_TARGET_C);
}

static void t_load_config_after_target(void)
{
    // load_config pulls a live target down to a lower stored ceiling.
    env_nvs_write("heat_max_c", HOST_NVS_U32, 4000);
    env->ctx = CTX_BOOT;
    CHECK(pb_heater_init() == ESP_OK);
    CHECK(pb_heater_set_target_c(65.0f) == ESP_OK);
    pb_heater_load_config();
    CHECK(pb_heater_get_target_c() == 40.0f);
}

static void t_method_nvs(void)
{
    env_nvs_write("heat_method", HOST_NVS_U8, 1);
    env_nvs_write("cool_rel_c", HOST_NVS_U32, 50);   // 0.5 C -> clamps to 30
    boot();
    CHECK(pb_heater_get_method() == PB_HEATER_METHOD_PID);
    CHECK(pb_heater_get_cool_release_c() == PB_HEATER_COOL_RELEASE_MIN_C);
}

static void t_method_nvs_corrupt(void)
{
    env_nvs_write("heat_method", HOST_NVS_U8, 7);
    env_nvs_write("cool_rel_c", HOST_NVS_U32, 99000);
    boot();
    CHECK(pb_heater_get_method() == PB_HEATER_METHOD_BANGBANG);
    CHECK(pb_heater_get_cool_release_c() == PB_HEATER_COOL_RELEASE_MAX_C);
}

static void t_cool_release_setter(void)
{
    boot();
    CHECK(pb_heater_set_cool_release_c(50.0f) == ESP_OK);
    CHECK(pb_heater_get_cool_release_c() == 50.0f);
    CHECK(nvs_u("cool_rel_c") == 5000);
    CHECK(pb_heater_set_cool_release_c(0.0f) == ESP_OK);
    CHECK(pb_heater_get_cool_release_c() == PB_HEATER_COOL_RELEASE_MIN_C);
    CHECK(pb_heater_set_cool_release_c(100.0f) == ESP_OK);
    CHECK(pb_heater_get_cool_release_c() == PB_HEATER_COOL_RELEASE_MAX_C);
    CHECK(pb_heater_set_cool_release_c(NAN) == ESP_ERR_INVALID_ARG);
    // A setter with NVS down still applies the RAM value (best effort persist).
    env->nvs_fail_open_rw = ESP_FAIL;
    CHECK(pb_heater_set_cool_release_c(45.0f) == ESP_OK);
    CHECK(pb_heater_set_max_target_c(50.0f) == ESP_OK);
    CHECK(pb_heater_set_comms_timeout_ms(20000) == ESP_OK);
    CHECK(pb_heater_set_fb_cut_c(100.0f) == ESP_OK);
    CHECK(pb_heater_set_method(PB_HEATER_METHOD_PID) == ESP_OK);
    CHECK(pb_heater_get_cool_release_c() == 45.0f);
    CHECK(pb_heater_get_max_target_c() == 50.0f);
    CHECK(nvs_u("heat_max_c") == -1);
    CHECK(pb_heater_set_method((pb_heater_method_t)5) == ESP_ERR_INVALID_ARG);
    CHECK(pb_heater_get_method() == PB_HEATER_METHOD_PID);
    env_check_quiescent();
}

// Persist the latch in phase 1, reboot, and check the restore in phase 2.
static void t_restore_phase1_trip(void)
{
    start_heating(60.0f);
    sensors(40.0f, 120.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);
}

static void t_restore_phase2_latched(void)
{
    env->level[PB_GPIO_RELAY] = 1;
    boot();
    CHECK(pb_heater_is_faulted());
    CHECK(pb_heater_fault_code() == PB_FAULT_PTC_OVERTEMP);
    CHECK(strcmp(pb_heater_fault_reason(), "PTC element over-temp") == 0);
    CHECK(set_target(40.0f) == ESP_ERR_INVALID_STATE);
    sensors(25.0f, 25.0f);
    link_alive();
    for (int i = 0; i < 10; ++i) { advance_ms(500); tick(); CHECK(relay() == 0); }
    // Explicit clear after recovery, then a fresh target heats again.
    CHECK(clear_fault() == ESP_OK);
    CHECK(nvs_u("fault_latch") == 0);
    tick(); CHECK(relay() == 0);                     // target was reset to 0
    CHECK(set_target(40.0f) == ESP_OK);
    tick(); CHECK(relay() == 1);
}

static void t_restore_phase3_clear_survives(void)
{
    boot();
    CHECK(!pb_heater_is_faulted());
}

static void t_restore_fresh_device(void)
{
    g_fh_seed_pass = false;
    boot();                                   // no namespace: not a fault
    CHECK(!pb_heater_is_faulted());
}

static void t_restore_ns_without_key(void)
{
    env_nvs_write("heat_max_c", HOST_NVS_U32, 6000);
    boot();
    CHECK(!pb_heater_is_faulted());
}

static void t_restore_open_error(void)
{
    env_nvs_write("fault_latch", HOST_NVS_U8, 0);
    env->nvs_fail_open_ro = ESP_ERR_NVS_NOT_INITIALIZED;
    boot();
    CHECK(pb_heater_is_faulted());
    CHECK(pb_heater_fault_code() == PB_FAULT_NVS_UNREADABLE);
    CHECK(set_target(40.0f) == ESP_ERR_INVALID_STATE);
}

static void t_restore_open_error_fresh(void)
{
    // An open error is not "not found", so it fails safe even on a new device.
    env->nvs_fail_open_ro = ESP_FAIL;
    boot();
    CHECK(pb_heater_is_faulted());
    CHECK(pb_heater_fault_code() == PB_FAULT_NVS_UNREADABLE);
}

static void t_restore_read_error(void)
{
    env_nvs_write("fault_latch", HOST_NVS_U8, 0);
    env->nvs_fail_get = ESP_FAIL;
    boot();
    CHECK(pb_heater_is_faulted());
    CHECK(pb_heater_fault_code() == PB_FAULT_NVS_UNREADABLE);
}

static void t_restore_corrupt_code(void)
{
    env_nvs_write("fault_latch", HOST_NVS_U8, 1);
    env_nvs_write("fault_code", HOST_NVS_U8, 200);
    boot();
    CHECK(pb_heater_is_faulted());
    CHECK(pb_heater_fault_code() == PB_FAULT_EMERGENCY);
}

static void t_restore_latch_code_none(void)
{
    env_nvs_write("fault_latch", HOST_NVS_U8, 7);
    env_nvs_write("fault_code", HOST_NVS_U8, 0);
    boot();
    CHECK(pb_heater_is_faulted());
    CHECK(pb_heater_fault_code() == PB_FAULT_EMERGENCY);
}

static void t_restore_each_code(void)
{
    for (int code = PB_FAULT_PTC_OVERTEMP; code < PB_FAULT__COUNT; ++code) {
        env_nvs_write("fault_latch", HOST_NVS_U8, 1);
        env_nvs_write("fault_code", HOST_NVS_U8, (uint32_t)code);
        boot();
        CHECK(pb_heater_is_faulted());
        CHECK(pb_heater_fault_code() == (pb_fault_reason_t)code);
        CHECK(clear_fault() == ESP_OK);
    }
}

static void t_panic_phase1(void)
{
    start_heating(60.0f);
    unsigned writes = env->writes[PB_GPIO_RELAY];
    env->ctx = CTX_OTHER;                      // the button task
    pb_heater_request_panic_off("button long-press");
    env_check_quiescent();
    CHECK(env->writes[PB_GPIO_RELAY] == writes);   // no GPIO write from that task
    CHECK(relay() == 1);                       // converges on the next tick
    CHECK(pb_heater_is_faulted());
    CHECK(!pb_heater_heat_mode());
    tick();
    CHECK(relay() == 0);
    CHECK(pb_heater_fault_code() == PB_FAULT_PANIC_OFF);
    CHECK(strcmp(pb_heater_fault_reason(), "button long-press") == 0);
    CHECK(nvs_u("fault_latch") == -1);         // panic-off is RAM only
    CHECK(set_target(50.0f) == ESP_ERR_INVALID_STATE);
}

static void t_panic_phase2_reboot_clears(void)
{
    boot();
    CHECK(!pb_heater_is_faulted());
}

static void t_panic_clear_then_fresh_target(void)
{
    start_heating(60.0f);
    pb_heater_request_panic_off(NULL);
    CHECK(strcmp(pb_heater_fault_reason(), "panic-off") == 0);
    tick(); CHECK(relay() == 0);
    CHECK(clear_fault() == ESP_OK);
    for (int i = 0; i < 5; ++i) { tick(); CHECK(relay() == 0); }   // no auto-resume
    CHECK(set_target(60.0f) == ESP_OK);
    tick(); CHECK(relay() == 1);
}

static void t_clear_fault_still_hot_relatches(void)
{
    start_heating(60.0f);
    sensors(40.0f, 110.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);
    CHECK(clear_fault() == ESP_OK);
    CHECK(!pb_heater_is_faulted());
    CHECK(nvs_u("fault_latch") == 0);
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);   // still hot: re-latch and re-persist
    // The same for the chamber.
    sensors(90.0f, 50.0f);
    CHECK(clear_fault() == ESP_OK);
    expect_trip(PB_FAULT_CHAMBER_OVERTEMP, true);
}

static void t_clear_fault_sensor_still_open(void)
{
    start_heating(60.0f);
    env->ntc_status[PB_NTC_CHAMBER] = PB_NTC_OPEN;
    expect_trip(PB_FAULT_CHAMBER_SENSOR, true);
    CHECK(clear_fault() == ESP_OK);
    // Target is 0 after a clear, so an idle open sensor is not a hazard...
    tick(); CHECK(!pb_heater_is_faulted() && relay() == 0);
    // ...but a new heat request trips again before the relay can turn on.
    link_alive();
    CHECK(set_target(60.0f) == ESP_OK);
    unsigned high = env->high_writes[PB_GPIO_RELAY];
    expect_trip(PB_FAULT_CHAMBER_SENSOR, true);
    CHECK(env->high_writes[PB_GPIO_RELAY] == high);
}

static void t_clear_fault_nvs_fails(void)
{
    start_heating(60.0f);
    sensors(40.0f, 110.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);
    sensors(30.0f, 40.0f);
    env->nvs_fail_open_rw = ESP_ERR_NVS_NOT_INITIALIZED;
    CHECK(clear_fault() == ESP_ERR_NVS_NOT_INITIALIZED);
    CHECK(pb_heater_is_faulted());
    env->nvs_fail_open_rw = 0;
    env->nvs_fail_set = ESP_FAIL;
    CHECK(clear_fault() == ESP_FAIL);
    CHECK(pb_heater_is_faulted());              // persist-first: stays latched
    CHECK(nvs_u("fault_latch") == 1);
    env->nvs_fail_set = 0;
    CHECK(clear_fault() == ESP_OK);
    CHECK(!pb_heater_is_faulted());
}

static void t_clear_fault_when_not_faulted(void)
{
    boot();
    CHECK(clear_fault() == ESP_OK);
    CHECK(nvs_u("fault_latch") == 0);
}

static void t_inhibit(void)
{
    start_heating(60.0f);
    env->ctx = CTX_CONTROL;
    pb_heater_inhibit("task watchdog unavailable");
    pb_heater_inhibit(NULL);
    env->ctx = CTX_OTHER;
    CHECK(relay() == 0);
    CHECK(pb_heater_is_inhibited() && pb_heater_is_faulted());
    CHECK(pb_heater_fault_code() == PB_FAULT_INHIBITED);
    CHECK(nvs_u("fault_latch") == -1);          // not persisted
    CHECK(clear_fault() == ESP_ERR_INVALID_STATE);
    CHECK(set_target(50.0f) == ESP_ERR_INVALID_STATE);
    for (int i = 0; i < 5; ++i) { link_alive(); tick(); CHECK(relay() == 0); }
}

static void t_emergency_off(void)
{
    start_heating(60.0f);
    env->ctx = CTX_CONTROL;
    pb_heater_emergency_off("policy watchdog");
    env->ctx = CTX_OTHER;
    CHECK(relay() == 0);
    CHECK(pb_heater_fault_code() == PB_FAULT_EMERGENCY);
    CHECK(nvs_u("fault_latch") == 1);
    CHECK(nvs_u("fault_code") == PB_FAULT_EMERGENCY);
    env->ctx = CTX_CONTROL;
    pb_heater_emergency_off(NULL);
    env->ctx = CTX_OTHER;
    CHECK(strcmp(pb_heater_fault_reason(), "safety trip") == 0);
}

static void t_deferred_persist(void)
{
    start_heating(60.0f);
    env->nvs_fail_open_rw = ESP_ERR_NVS_NOT_INITIALIZED;
    sensors(40.0f, 108.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, false);
    CHECK(nvs_u("fault_latch") == -1);          // the write failed
    // The tick retries. NVS stays down: nothing lands.
    for (int i = 0; i < 8; ++i) { advance_ms(500); tick(); }
    CHECK(nvs_u("fault_latch") == -1);
    env->nvs_fail_open_rw = 0;
    // The next retry is at most 2 s after the last attempt.
    sensors(30.0f, 40.0f);
    advance_ms(2000);
    tick();
    CHECK(nvs_u("fault_latch") == 1);
    CHECK(nvs_u("fault_code") == PB_FAULT_PTC_OVERTEMP);
    unsigned sets = env->nvs_sets;
    for (int i = 0; i < 8; ++i) { advance_ms(500); tick(); }
    CHECK(env->nvs_sets == sets);         // nothing pending any more
}

static void t_deferred_persist_partial(void)
{
    // The latch key lands, the code key fails: flash is latched (fail-safe)
    // and the retry writes the code later.
    start_heating(60.0f);
    env->nvs_fail_set_nth = 2;
    sensors(40.0f, 107.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, false);
    CHECK(nvs_u("fault_latch") == 1);
    CHECK(nvs_u("fault_code") == -1);
    env->nvs_fail_set_nth = 0;
    advance_ms(2500);
    tick();
    CHECK(nvs_u("fault_code") == PB_FAULT_PTC_OVERTEMP);
}

static void t_deferred_persist_throttle_and_lock(void)
{
    advance_ms(5000);                           // the first retry waits for 2 s uptime
    start_heating(60.0f);
    env->nvs_fail_set = ESP_FAIL;
    sensors(86.0f, 60.0f);
    expect_trip(PB_FAULT_CHAMBER_OVERTEMP, false);
    advance_ms(100);
    tick();                                     // first retry (no retry before)
    env->nvs_fail_set = 0;
    unsigned sets = env->nvs_sets;
    advance_ms(1000);
    tick();                                     // < 2 s since the retry: throttled
    CHECK(env->nvs_sets == sets);
    CHECK(nvs_u("fault_latch") == -1);
    advance_ms(1000);
    env->sem_contended = true;                  // a clear in another task holds the lock
    tick();
    CHECK(nvs_u("fault_latch") == -1);
    env->sem_contended = false;
    tick();
    CHECK(nvs_u("fault_latch") == 1);
}

static void race_clear_hook(void)
{
    env->nvs_fail_set = 0;                      // NVS recovers
    host_ctx_t saved = env->ctx;
    env->ctx = CTX_OTHER;
    CHECK(pb_heater_clear_fault() == ESP_OK);
    env->ctx = saved;
}

static void t_deferred_persist_clear_race(void)
{
    // A clear from the HTTP task lands between the failed latch persist and the
    // "pending" flag. The retry must not write a stale "latched" afterwards.
    start_heating(60.0f);
    env->nvs_fail_set = ESP_FAIL;
    env->after_give_hook = race_clear_hook;
    sensors(40.0f, 60.0f);
    env->ctx = CTX_CONTROL;
    pb_heater_emergency_off("race");
    env->ctx = CTX_OTHER;
    CHECK(!pb_heater_is_faulted());
    CHECK(nvs_u("fault_latch") == 0);
    advance_ms(3000);
    tick();
    CHECK(nvs_u("fault_latch") == 0);
    unsigned sets = env->nvs_sets;
    advance_ms(3000);
    tick();
    CHECK(env->nvs_sets == sets);
}

static void t_deferred_persist_then_clear(void)
{
    start_heating(60.0f);
    env->nvs_fail_set = ESP_FAIL;
    sensors(40.0f, 106.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, false);
    env->nvs_fail_set = 0;
    sensors(30.0f, 40.0f);
    CHECK(clear_fault() == ESP_OK);             // clears the pending retry too
    advance_ms(5000);
    tick();
    CHECK(nvs_u("fault_latch") == 0);
}

static void t_pid_heats_and_obeys_safety(void)
{
    boot();
    env->ctx = CTX_OTHER;
    CHECK(pb_heater_set_method(PB_HEATER_METHOD_PID) == ESP_OK);
    CHECK(nvs_u("heat_method") == 1);
    sensors(30.0f, 60.0f);
    link_alive();
    CHECK(set_target(60.0f) == ESP_OK);
    tick(); CHECK(relay() == 1);
    pb_heater_telemetry_t tel;
    pb_heater_get_telemetry(&tel);
    CHECK(tel.constraint == PB_HEATER_CONSTRAINT_NONE && tel.commanded_duty > 0.9f);
    // Element foldback cuts the relay in PID mode too.
    sensors(30.0f, 102.0f); advance_ms(500); tick(); CHECK(relay() == 0);
    pb_heater_get_telemetry(&tel);
    CHECK(tel.constraint == PB_HEATER_CONSTRAINT_ELEMENT_FOLDBACK && tel.commanded_duty == 0.0f);
    sensors(30.0f, 90.0f); advance_ms(500); tick(); CHECK(relay() == 1);
    // At target: off.
    sensors(60.0f, 90.0f); advance_ms(500); tick(); CHECK(relay() == 0);
    pb_heater_get_telemetry(&tel);
    CHECK(tel.constraint == PB_HEATER_CONSTRAINT_TARGET_REACHED);
    // Local foldback with external regulation.
    pb_heater_set_control_chamber_c(40.0f);
    sensors(75.0f, 90.0f); advance_ms(500); tick(); CHECK(relay() == 0);
    pb_heater_get_telemetry(&tel);
    CHECK(tel.constraint == PB_HEATER_CONSTRAINT_LOCAL_FOLDBACK);
    pb_heater_set_control_chamber_c(NAN);
    // Trips are the same in PID mode.
    sensors(30.0f, 60.0f); advance_ms(500); tick();
    sensors(30.0f, 105.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);
}

static void t_pid_approach_shaping(void)
{
    // The soft approach cap engages while the chamber climbs fast near the
    // target. The relay drive stays within the cap (window duty <= cap).
    boot();
    CHECK(pb_heater_set_method(PB_HEATER_METHOD_PID) == ESP_OK);
    link_alive();
    CHECK(set_target(60.0f) == ESP_OK);
    float min_cap = 1.0f;
    pb_heater_telemetry_t tel;
    for (float c = 50.0f; c < 60.0f; c += 0.25f) {
        sensors(c, 70.0f);
        advance_ms(500);
        tick();
        pb_heater_get_telemetry(&tel);
        CHECK(tel.approach_limit > 0.0f && tel.approach_limit <= 1.0f);
        CHECK(tel.commanded_duty <= tel.approach_limit);
        if (tel.approach_limit < min_cap) min_cap = tel.approach_limit;
    }
    CHECK(min_cap < 0.75f);
}

static void t_pid_error_forces_off(void)
{
    boot();
    CHECK(pb_heater_set_method(PB_HEATER_METHOD_PID) == ESP_OK);
    sensors(30.0f, 60.0f);
    link_alive();
    CHECK(set_target(60.0f) == ESP_OK);
    // Extreme external values make the PID derivative overflow.
    pb_heater_set_control_chamber_c(-3.0e38f);
    tick();
    pb_heater_set_control_chamber_c(3.0e38f);
    advance_ms(500);
    tick();
    pb_heater_set_control_chamber_c(-3.0e38f);
    advance_ms(500);
    tick();
    pb_heater_telemetry_t tel;
    pb_heater_get_telemetry(&tel);
    CHECK(tel.constraint == PB_HEATER_CONSTRAINT_PID_ERROR);
    CHECK(relay() == 0);
    CHECK(!pb_heater_is_faulted());
}

static void t_method_switch_mid_heat(void)
{
    start_heating(60.0f);
    CHECK(pb_heater_set_method(PB_HEATER_METHOD_PID) == ESP_OK);
    advance_ms(500); tick(); CHECK(relay() == 1);
    CHECK(pb_heater_set_method(PB_HEATER_METHOD_BANGBANG) == ESP_OK);
    advance_ms(500); tick(); CHECK(relay() == 1);
    sensors(60.0f, 60.0f); advance_ms(500); tick(); CHECK(relay() == 0);
}

static void t_before_init(void)
{
    // Calls before pb_heater_init() (no persist mutex yet) still persist.
    env->ctx = CTX_BOOT;
    pb_heater_emergency_off("early");
    CHECK(nvs_u("fault_latch") == 1);
    CHECK(pb_heater_clear_fault() == ESP_OK);
    CHECK(nvs_u("fault_latch") == 0);
    env->nvs_fail_set = ESP_FAIL;
    pb_heater_emergency_off("early, NVS down");
    CHECK(pb_heater_clear_fault() == ESP_FAIL);
    CHECK(pb_heater_is_faulted());
    advance_ms(5000);
    env->ctx = CTX_CONTROL;
    pb_heater_tick();                           // no mutex: the retry waits
    CHECK(nvs_u("fault_latch") == 0);
    env_check_quiescent();
    env->nvs_fail_set = 0;
    boot();
}

static void t_strings(void)
{
    CHECK(strcmp(pb_heater_fault_str(PB_FAULT_NONE), "none") == 0);
    CHECK(strcmp(pb_heater_fault_str(PB_FAULT__COUNT), "latched fault") == 0);
    CHECK(strcmp(pb_heater_fault_str((pb_fault_reason_t)-1), "latched fault") == 0);
    static const char *const names[] = {
        "off", "none", "approach_limit", "target_reached", "local_foldback",
        "element_foldback", "pid_error",
    };
    for (int i = 0; i <= PB_HEATER_CONSTRAINT_PID_ERROR; ++i)
        CHECK(strcmp(pb_heater_constraint_str((pb_heater_constraint_t)i), names[i]) == 0);
    CHECK(strcmp(pb_heater_constraint_str((pb_heater_constraint_t)99), "unknown") == 0);
}

// --- fault persistence across causes, and the approach label --------------------

// A hazard trip on top of a RAM-only latch (panic-off) is written to NVS.
// SAFETY.md: a hazard-driven trip is written to NVS and restored at boot.
static void t_hazard_during_panic_persisted_phase1(void)
{
    start_heating(60.0f);
    pb_heater_request_panic_off("button");
    tick();
    CHECK(nvs_u("fault_latch") == -1);         // panic-off alone stays RAM-only
    sensors(40.0f, 120.0f);                    // element over-temp while panic-latched
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);
    // While it stays hot, the repeated trips do not write NVS again.
    unsigned sets = env->nvs_sets;
    for (int i = 0; i < 10; ++i) { advance_ms(500); tick(); }
    CHECK(env->nvs_sets == sets);
    // A later panic-off does not replace the persisted cause.
    pb_heater_request_panic_off("button again");
    tick();
    CHECK(nvs_u("fault_code") == PB_FAULT_PTC_OVERTEMP);
}

static void t_hazard_during_panic_persisted_phase2(void)
{
    boot();
    CHECK(pb_heater_is_faulted());
    CHECK(pb_heater_fault_code() == PB_FAULT_PTC_OVERTEMP);
}

static void t_hazard_during_inhibit_persisted(void)
{
    start_heating(60.0f);
    env->ctx = CTX_CONTROL;
    pb_heater_inhibit("watchdog");
    env->ctx = CTX_OTHER;
    CHECK(nvs_u("fault_latch") == -1);
    sensors(90.0f, 60.0f);
    expect_trip(PB_FAULT_CHAMBER_OVERTEMP, true);
}

static void t_hazard_after_unreadable_boot_persisted(void)
{
    env->nvs_fail_open_ro = ESP_FAIL;          // boot latch: NVS_UNREADABLE
    boot();
    CHECK(pb_heater_fault_code() == PB_FAULT_NVS_UNREADABLE);
    sensors(25.0f, 106.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);
}

static void t_restored_latch_not_rewritten(void)
{
    env_nvs_write("fault_latch", HOST_NVS_U8, 1);
    env_nvs_write("fault_code", HOST_NVS_U8, PB_FAULT_CHAMBER_OVERTEMP);
    boot();
    unsigned sets = env->nvs_sets;
    sensors(90.0f, 40.0f);
    tick();                                    // a trip on top of a restored latch
    CHECK(env->nvs_sets == sets);
    CHECK(nvs_u("fault_code") == PB_FAULT_CHAMBER_OVERTEMP);
}

static void t_hazard_during_panic_deferred(void)
{
    // NVS is down when the hazard trips on top of panic-off. The retry writes
    // the hazard code, not the panic-off code.
    start_heating(60.0f);
    pb_heater_request_panic_off("button");
    tick();
    env->nvs_fail_set = ESP_FAIL;
    sensors(40.0f, 120.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, false);
    pb_heater_request_panic_off("button again");
    env->nvs_fail_set = 0;
    sensors(30.0f, 40.0f);
    advance_ms(3000);
    tick();
    CHECK(nvs_u("fault_latch") == 1);
    CHECK(nvs_u("fault_code") == PB_FAULT_PTC_OVERTEMP);
}

static void race_clear_then_panic_hook(void)
{
    env->nvs_fail_set = 0;                      // NVS recovers
    host_ctx_t saved = env->ctx;
    env->ctx = CTX_OTHER;
    CHECK(pb_heater_clear_fault() == ESP_OK);   // HTTP task: clear
    pb_heater_request_panic_off("button");      // button task: panic-off
    env->ctx = saved;
}

static void t_retry_after_clear_and_panic(void)
{
    // A hazard trip fails to write NVS. Before the control task continues, a
    // clear lands and then a panic-off. The retry must not write the cleared
    // hazard back into NVS: the panic-off latch is RAM-only.
    start_heating(60.0f);
    env->nvs_fail_set = ESP_FAIL;
    env->after_give_hook = race_clear_then_panic_hook;
    sensors(40.0f, 107.0f);
    tick();
    CHECK(pb_heater_is_faulted() && pb_heater_fault_code() == PB_FAULT_PANIC_OFF);
    CHECK(nvs_u("fault_latch") == 0);
    sensors(30.0f, 40.0f);
    for (int i = 0; i < 10; ++i) { advance_ms(1000); tick(); }
    CHECK(nvs_u("fault_latch") == 0);
}

static void t_hazard_replaces_emergency_once(void)
{
    // emergency_off (policy watchdog) persists code 8. A later hazard replaces
    // it in NVS once. Later causes do not write NVS again.
    start_heating(60.0f);
    env->ctx = CTX_CONTROL;
    pb_heater_emergency_off("lease watchdog");
    env->ctx = CTX_OTHER;
    CHECK(nvs_u("fault_code") == PB_FAULT_EMERGENCY);
    sensors(40.0f, 108.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);
    unsigned sets = env->nvs_sets;
    for (int i = 0; i < 5; ++i) { advance_ms(500); tick(); }      // still hot
    sensors(90.0f, 108.0f);
    advance_ms(500); tick();                                       // another hazard
    env->ctx = CTX_CONTROL;
    pb_heater_emergency_off("lease watchdog");                    // non-hazard on top
    env->ctx = CTX_OTHER;
    CHECK(env->nvs_sets == sets);
    CHECK(nvs_u("fault_code") == PB_FAULT_PTC_OVERTEMP);
}

static void t_hazard_replaces_restored_emergency(void)
{
    env_nvs_write("fault_latch", HOST_NVS_U8, 1);
    env_nvs_write("fault_code", HOST_NVS_U8, PB_FAULT_EMERGENCY);
    boot();
    sensors(86.0f, 40.0f);
    expect_trip(PB_FAULT_CHAMBER_OVERTEMP, true);
}

// The "approach_limit" constraint is reported while the soft approach cap is
// active.
static void t_approach_limit_label(void)
{
    boot();
    CHECK(pb_heater_set_method(PB_HEATER_METHOD_PID) == ESP_OK);
    link_alive();
    bool labelled = false;
    pb_heater_telemetry_t tel;
    CHECK(set_target(60.0f) == ESP_OK);
    for (int i = 0; i < 600; ++i) { sensors(56.0f, 70.0f); advance_ms(500); link_alive(); tick(); }
    for (float c = 56.0f; c < 60.0f; c += 0.05f) {
        sensors(c, 70.0f);
        advance_ms(500);
        link_alive();
        tick();
        pb_heater_get_telemetry(&tel);
        if (tel.constraint == PB_HEATER_CONSTRAINT_APPROACH_LIMIT) {
            labelled = true;
            CHECK(tel.approach_limit < 1.0f);
            CHECK(tel.commanded_duty <= tel.approach_limit);
        }
        if (tel.approach_limit < 1.0f)       // c < 60 C: no constraint ranks higher
            CHECK(tel.constraint == PB_HEATER_CONSTRAINT_APPROACH_LIMIT);
    }
    CHECK(labelled);
}

// A clear that fails part way leaves flash latched. ESP-IDF writes each set
// through to flash, so the order of the keys matters.
static void partial_clear_case(int nth)
{
    start_heating(60.0f);
    sensors(40.0f, 110.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);
    sensors(30.0f, 40.0f);
    env->nvs_fail_set_nth = nth;
    CHECK(clear_fault() != ESP_OK);
    CHECK(pb_heater_is_faulted());
    CHECKF(nvs_u("fault_latch") == 1,
           "failed clear left flash unlatched (fault_latch=%d) while RAM is latched",
           nvs_u("fault_latch"));
}

static void t_partial_clear_first_set_fails(void)  { partial_clear_case(1); }
static void t_partial_clear_second_set_fails(void)
{
    partial_clear_case(2);
    // The code key is already cleared: the next boot restores a generic latch.
    env->nvs_fail_set_nth = 0;
    CHECK(nvs_u("fault_code") == PB_FAULT_NONE);
}

static void t_partial_clear_retry_restores_cause(void)
{
    partial_clear_case(2);
    env->nvs_fail_set_nth = 0;
    advance_ms(2500);
    tick();                                    // the retry writes the cause again
    CHECK(nvs_u("fault_latch") == 1);
    CHECK(nvs_u("fault_code") == PB_FAULT_PTC_OVERTEMP);
}

static void t_partial_clear_phase2(void)
{
    boot();
    CHECK(pb_heater_is_faulted());
    CHECK(pb_heater_fault_code() == PB_FAULT_EMERGENCY);
}

// --- metrics counters (pb_heater_get_counters) ---------------------------------

static pb_heater_counters_t counters(void)
{
    pb_heater_counters_t c;
    memset(&c, 0xA5, sizeof c);
    env->ctx = CTX_OTHER;
    pb_heater_get_counters(&c);
    env_check_quiescent();
    return c;
}

static uint32_t trips_total(const pb_heater_counters_t *c)
{
    uint32_t n = 0;
    for (int i = 0; i < PB_FAULT__COUNT; ++i) n += c->trips[i];
    return n;
}

#define CHECK_TRIPS(c, code, n) \
    CHECKF((c).trips[code] == (n), "trips[%s] = %u, want %u", #code, \
           (unsigned)(c).trips[code], (unsigned)(n))

// Phase 1: a held over-temperature counts once, however many ticks it holds.
static void t_counters_held_trip_phase1(void)
{
    start_heating(60.0f);
    pb_heater_counters_t c = counters();
    CHECK(trips_total(&c) == 0 && c.panic_offs == 0);
    sensors(40.0f, 110.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);
    for (int i = 0; i < 10; ++i) { advance_ms(500); tick(); }
    c = counters();
    CHECK_TRIPS(c, PB_FAULT_PTC_OVERTEMP, 1);
    CHECK(trips_total(&c) == 1);
}

// Phase 2 (reboot, NVS kept): the restore of the persisted latch does not
// count. A hazard that is still present trips live and counts once.
static void t_counters_restore_phase2(void)
{
    boot();
    CHECK(pb_heater_is_faulted());
    CHECK(pb_heater_fault_code() == PB_FAULT_PTC_OVERTEMP);
    pb_heater_counters_t c = counters();
    CHECKF(trips_total(&c) == 0, "boot restore counted %u trips", (unsigned)trips_total(&c));
    sensors(25.0f, 25.0f);
    for (int i = 0; i < 4; ++i) { advance_ms(500); tick(); }
    c = counters();
    CHECK(trips_total(&c) == 0);              // restored latch, no live hazard
    sensors(40.0f, 110.0f);
    for (int i = 0; i < 4; ++i) { advance_ms(500); tick(); }
    c = counters();
    CHECK_TRIPS(c, PB_FAULT_PTC_OVERTEMP, 1);
    CHECK(trips_total(&c) == 1);
}

// A new cause during an active latch counts. A repeated cause does not.
static void t_counters_new_cause_during_latch(void)
{
    start_heating(60.0f);
    env->ctx = CTX_OTHER;
    pb_heater_request_panic_off("button");
    tick();
    pb_heater_counters_t c = counters();
    CHECK(c.panic_offs == 1 && trips_total(&c) == 0);   // panic-off has its own counter
    sensors(40.0f, 110.0f);                    // hazard while the panic latch is active
    tick();
    c = counters();
    CHECK_TRIPS(c, PB_FAULT_PTC_OVERTEMP, 1);
    // A second panic-off rewrites the cause. The PTC trip on the next tick is
    // the same cause in the same episode: it does not count again.
    pb_heater_request_panic_off(NULL);
    tick();
    tick();
    c = counters();
    CHECK(c.panic_offs == 2);
    CHECK_TRIPS(c, PB_FAULT_PTC_OVERTEMP, 1);
    // A third cause in the same episode.
    env->ctx = CTX_CONTROL;
    pb_heater_emergency_off("policy watchdog");
    pb_heater_emergency_off("policy watchdog");
    env->ctx = CTX_OTHER;
    c = counters();
    CHECK_TRIPS(c, PB_FAULT_EMERGENCY, 1);
    CHECK(trips_total(&c) == 2);
}

// A successful clear starts a new episode. A failed clear does not.
static void t_counters_clear_episode(void)
{
    start_heating(60.0f);
    sensors(40.0f, 110.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);
    CHECK_TRIPS(counters(), PB_FAULT_PTC_OVERTEMP, 1);

    // Failed clears: the latch stays, the held hazard does not count again.
    env->nvs_fail_set = ESP_FAIL;
    CHECK(clear_fault() == ESP_FAIL);
    env->nvs_fail_set = 0;
    env->nvs_fail_open_rw = ESP_ERR_NVS_NOT_INITIALIZED;
    CHECK(clear_fault() == ESP_ERR_NVS_NOT_INITIALIZED);
    env->nvs_fail_open_rw = 0;
    CHECK(pb_heater_is_faulted());
    for (int i = 0; i < 4; ++i) { advance_ms(500); tick(); }
    CHECK_TRIPS(counters(), PB_FAULT_PTC_OVERTEMP, 1);

    // Successful clear while still hot: the next tick is a new episode.
    CHECK(clear_fault() == ESP_OK);
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);
    for (int i = 0; i < 4; ++i) { advance_ms(500); tick(); }
    CHECK_TRIPS(counters(), PB_FAULT_PTC_OVERTEMP, 2);

    // Cool down, clear, heat again, trip again: a third episode.
    sensors(30.0f, 40.0f);
    tick();
    CHECK(clear_fault() == ESP_OK);
    link_alive();
    CHECK(set_target(60.0f) == ESP_OK);
    tick(); CHECK(relay() == 1);
    sensors(40.0f, 110.0f);
    expect_trip(PB_FAULT_PTC_OVERTEMP, true);
    pb_heater_counters_t c = counters();
    CHECK_TRIPS(c, PB_FAULT_PTC_OVERTEMP, 3);
    CHECK(trips_total(&c) == 3);
}

// Each panic-off request counts: while heating, while latched, while idle.
static void t_counters_panic_each_request(void)
{
    boot();
    env->ctx = CTX_OTHER;
    pb_heater_request_panic_off(NULL);        // idle, not heating
    CHECK(counters().panic_offs == 1);
    pb_heater_request_panic_off(NULL);        // already latched
    pb_heater_request_panic_off(NULL);
    tick();
    CHECK(clear_fault() == ESP_OK);
    pb_heater_request_panic_off(NULL);        // after a clear
    pb_heater_counters_t c = counters();
    CHECK(c.panic_offs == 4);
    CHECK(trips_total(&c) == 0);
}

// Relay on-time: exact values, and never a decrease between two reads, also
// at the same clock value as a relay change.
static uint64_t last_on_us;

static uint64_t on_us(void)
{
    uint64_t v = counters().relay_on_us;
    CHECKF(v >= last_on_us, "relay on-time decreased: %llu -> %llu",
           (unsigned long long)last_on_us, (unsigned long long)v);
    last_on_us = v;
    return v;
}

static void t_counters_relay_on_time(void)
{
    last_on_us = 0;
    boot();
    sensors(30.0f, 60.0f);
    link_alive();
    CHECK(on_us() == 0);
    CHECK(set_target(60.0f) == ESP_OK);
    tick(); CHECK(relay() == 1);              // on at t0
    CHECK(on_us() == 0);                      // same clock value: open period 0
    advance_ms(500);
    CHECK(on_us() == 500000);
    link_alive();
    tick(); CHECK(relay() == 1);              // still on
    CHECK(on_us() == 500000);
    CHECK(set_target(0.0f) == ESP_OK);
    tick(); CHECK(relay() == 0);              // off at t0 + 500 ms
    CHECK(on_us() == 500000);
    advance_ms(1000);
    CHECK(on_us() == 500000);                 // off time does not count
    link_alive();
    CHECK(set_target(60.0f) == ESP_OK);
    tick(); CHECK(relay() == 1);              // on at t0 + 1500 ms
    CHECK(on_us() == 500000);
    advance_ms(250);
    CHECK(on_us() == 750000);
    // Many toggles every 100 ms, with a read before and after each tick at the
    // same clock value. The relay is on in each 100 ms step that starts on.
    uint64_t want = 750000;
    for (int i = 0; i < 20; ++i) {
        bool was_on = relay() == 1;
        advance_ms(100);
        if (was_on) want += 100000;
        link_alive();
        CHECK(set_target((i & 1) ? 60.0f : 0.0f) == ESP_OK);
        CHECK(on_us() == want);
        tick();
        CHECK(relay() == (i & 1));
        CHECK(on_us() == want);
    }
    CHECK(want == 750000 + 100000 + 9 * 100000);   // 10 on steps of 100 ms
}

// The control task turns the relay on between the clock read and the lock of
// a reader (a task switch). The open period must not be negative.
static void relay_on_hook(void)
{
    advance_ms(1);
    link_alive();
    CHECK(set_target(60.0f) == ESP_OK);
    tick();
    CHECK(relay() == 1);
}

static void t_counters_relay_on_race(void)
{
    last_on_us = 0;
    start_heating(60.0f);                     // on at t0
    advance_ms(300);
    CHECK(set_target(0.0f) == ESP_OK);
    tick(); CHECK(relay() == 0);              // 300 ms on
    advance_ms(200);
    CHECK(on_us() == 300000);
    env->before_crit_hook = relay_on_hook;    // the switch occurs inside the next read
    CHECK(on_us() == 300000);
    CHECK(env->before_crit_hook == NULL);
    advance_ms(100);
    CHECK(on_us() == 400000);
}

int main(int argc, char **argv)
{
    env_init();
    bool ok = true;
#define RUN(fn)      do { env_reset(); ok &= env_run(#fn, fn); } while (0)
#define RUN_KEEP(fn) do { ok &= env_run(#fn, fn); } while (0)

    bool only_random = argc > 1 && strcmp(argv[1], "random") == 0;
    if (!only_random) {
        RUN(t_boot_relay_off);
        RUN(t_boot_gpio_config_fails);
        RUN(t_basic_bangbang);
        RUN(t_ptc_overtemp_latches);
        RUN(t_ptc_overtemp_trips_when_idle);
        RUN(t_foldback_82k);
        RUN(t_foldback_33k);
        RUN(t_foldback_user_override);
        RUN(t_foldback_nvs_clamped);
        RUN(t_foldback_nvs_low_clamped);
        RUN(t_chamber_overtemp);
        RUN(t_chamber_overtemp_external_regulation);
        RUN(t_local_foldback_external);
        RUN(t_sensor_fault_chamber_open);
        RUN(t_sensor_fault_chamber_short);
        RUN(t_sensor_fault_chamber_uninit);
        RUN(t_sensor_fault_ptc_open);
        RUN(t_sensor_fault_ptc_short);
        RUN(t_sensor_fault_ptc_uninit);
        RUN(t_sensor_fault_idle_then_arm);
        RUN(t_comms_watchdog);
        RUN(t_comms_watchdog_default_and_max);
        RUN(t_comms_idle_stale_link);
        RUN(t_comms_nvs_clamped);
        RUN(t_comms_nvs_high_clamped);
        RUN(t_target_clamp);
        RUN(t_target_clamp_nvs);
        RUN(t_target_clamp_nvs_low);
        RUN(t_load_config_after_target);
        RUN(t_method_nvs);
        RUN(t_method_nvs_corrupt);
        RUN(t_cool_release_setter);
        RUN(t_restore_phase1_trip);
        RUN_KEEP(t_restore_phase2_latched);
        RUN_KEEP(t_restore_phase3_clear_survives);
        RUN(t_restore_fresh_device);
        RUN(t_restore_ns_without_key);
        RUN(t_restore_open_error);
        RUN(t_restore_open_error_fresh);
        RUN(t_restore_read_error);
        RUN(t_restore_corrupt_code);
        RUN(t_restore_latch_code_none);
        RUN(t_restore_each_code);
        RUN(t_panic_phase1);
        RUN_KEEP(t_panic_phase2_reboot_clears);
        RUN(t_panic_clear_then_fresh_target);
        RUN(t_clear_fault_still_hot_relatches);
        RUN(t_clear_fault_sensor_still_open);
        RUN(t_clear_fault_nvs_fails);
        RUN(t_clear_fault_when_not_faulted);
        RUN(t_inhibit);
        RUN(t_emergency_off);
        RUN(t_deferred_persist);
        RUN(t_deferred_persist_partial);
        RUN(t_deferred_persist_throttle_and_lock);
        RUN(t_deferred_persist_clear_race);
        RUN(t_deferred_persist_then_clear);
        RUN(t_pid_heats_and_obeys_safety);
        RUN(t_pid_approach_shaping);
        RUN(t_pid_error_forces_off);
        RUN(t_method_switch_mid_heat);
        RUN(t_before_init);
        RUN(t_strings);
        RUN(t_hazard_during_panic_persisted_phase1);
        RUN_KEEP(t_hazard_during_panic_persisted_phase2);
        RUN(t_hazard_during_inhibit_persisted);
        RUN(t_hazard_after_unreadable_boot_persisted);
        RUN(t_restored_latch_not_rewritten);
        RUN(t_hazard_during_panic_deferred);
        RUN(t_retry_after_clear_and_panic);
        RUN(t_hazard_replaces_emergency_once);
        RUN(t_hazard_replaces_restored_emergency);
        RUN(t_approach_limit_label);
        RUN(t_partial_clear_first_set_fails);
        RUN(t_partial_clear_retry_restores_cause);
        RUN(t_partial_clear_second_set_fails);
        RUN_KEEP(t_partial_clear_phase2);
        RUN(t_counters_held_trip_phase1);
        RUN_KEEP(t_counters_restore_phase2);
        RUN(t_counters_new_cause_during_latch);
        RUN(t_counters_clear_episode);
        RUN(t_counters_panic_each_request);
        RUN(t_counters_relay_on_time);
        RUN(t_counters_relay_on_race);
        ok &= heater_first_heat_tests();
    }

    ok &= heater_random_test();

    if (!ok) {
        fprintf(stderr, "pb_heater tick host test: FAILED\n");
        return 1;
    }
    printf("pb_heater tick host test: all passed\n");
    return 0;
}
