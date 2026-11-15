// SPDX-License-Identifier: MIT
// Host test for the REAL pb_ntc.c (ADC -> temperature) and for the chain
// pb_ntc.c -> pb_heater.c. The ADC driver, the board strap, NVS and the relay
// GPIO are stubs (tests/stubs_heater/, tests/heater_host_env.c).
//
//  - Conversion against docs/NTC_CONVERSION.md for both reference resistors
//    (82 kOhm and 33 kOhm), the table clamps, and the open/short limits.
//  - swap_thermistors: the logical channels, offsets and cutoffs follow the
//    swap that is latched at boot.
//  - SAFETY.md, "Sensor calibration is bounded": an offset (set or loaded from
//    NVS) moves the real cutoff temperature by at most 5 C and cannot hide a
//    sensor fault.
#define _DEFAULT_SOURCE
#include "heater_host_env.h"
#include "pb_board.h"
#include "pb_heater.h"
#include "pb_ntc.h"

#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- ADC and board stubs -------------------------------------------------------------

int pb_board_rref_kohm(void) { return env->rref_kohm; }

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg,
                               adc_oneshot_unit_handle_t *ret)
{
    CHECK(cfg->unit_id == ADC_UNIT_1);
    *ret = (adc_oneshot_unit_handle_t)(uintptr_t)0x1;
    return env->adc_new_unit_err;
}

esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, adc_channel_t ch,
                                     const adc_oneshot_chan_cfg_t *cfg)
{
    (void)h;
    CHECK(ch == ADC_CHANNEL_0 || ch == ADC_CHANNEL_1);
    CHECK(cfg->atten == ADC_ATTEN_DB_12 && cfg->bitwidth == ADC_BITWIDTH_12);
    return env->adc_config_err;
}

esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg,
                                               adc_cali_handle_t *ret)
{
    // The handle carries the channel, so the voltage stub knows which pin it is.
    *ret = (adc_cali_handle_t)(uintptr_t)(cfg->chan + 1);
    return env->adc_cali_create_err;
}

esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t ch, int *raw)
{
    (void)h;
    CHECK((int)ch >= 0 && (int)ch < HOST_ADC_CHANNELS);
    if (env->adc_read_err[ch]) return env->adc_read_err[ch];
    *raw = env->adc_raw[ch];
    return ESP_OK;
}

esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *mv)
{
    int ch = (int)(uintptr_t)h - 1;
    CHECK(ch >= 0 && ch < HOST_ADC_CHANNELS);
    CHECK(raw == env->adc_raw[ch]);
    if (env->adc_cali_err) return env->adc_cali_err;
    *mv = env->adc_mv[ch];
    return ESP_OK;
}

// --- reference model from docs/NTC_CONVERSION.md ------------------------------------------

// R/T table, section 3 of the doc: temp_C 12..125 -> Rntc (kOhm).
static const float k_doc_r[] = {
    198.7f,189.4f,180.7f,172.4f,164.5f,157.0f,149.9f,143.2f,136.8f,130.7f,
    124.9f,119.4f,114.2f,109.2f,104.5f,100.0f, 95.7f, 91.6f, 87.8f, 84.1f,
     80.6f, 77.2f, 74.0f, 70.9f, 68.0f, 65.3f, 62.6f, 60.1f, 57.7f, 55.4f,
     53.2f, 51.1f, 49.1f, 47.2f, 45.3f, 43.6f, 41.9f, 40.3f, 38.8f, 37.3f,
     35.9f, 34.5f, 33.2f, 32.0f, 30.8f, 29.7f, 28.6f, 27.6f, 26.6f, 25.6f,
     24.7f, 23.8f, 23.0f, 22.2f, 21.4f, 20.6f, 19.9f, 19.2f, 18.6f, 17.9f,
     17.3f, 16.7f, 16.2f, 15.6f, 15.1f, 14.6f, 14.1f, 13.6f, 13.2f, 12.8f,
     12.4f, 12.0f, 11.6f, 11.2f, 10.9f, 10.5f, 10.2f,  9.9f,  9.6f,  9.3f,
      9.0f,  8.7f,  8.4f,  8.2f,  7.9f,  7.7f,  7.4f,  7.2f,  7.0f,  6.8f,
      6.6f,  6.4f,  6.2f,  6.0f,  5.9f,  5.7f,  5.5f,  5.4f,  5.2f,  5.1f,
      4.9f,  4.8f,  4.7f,  4.5f,  4.4f,  4.3f,  4.2f,  4.1f,  3.9f,  3.8f,
      3.7f,  3.6f,  3.5f,  3.4f,
};
#define DOC_T0 12
#define DOC_N ((int)(sizeof(k_doc_r) / sizeof(k_doc_r[0])))

// Rntc = Rref * V / (Vsupply - V), Vsupply = 3.3 V (hardware correction).
static double doc_r_kohm(int mv, int rref) { double v = mv / 1000.0; return rref * v / (3.3 - v); }

// Stock firmware: nearest table entry.
static double doc_nearest_c(double r)
{
    int best = 0;
    for (int i = 1; i < DOC_N; ++i)
        if (fabs(k_doc_r[i] - r) < fabs(k_doc_r[best] - r)) best = i;
    return DOC_T0 + best;
}

// Resistance for a true temperature, linear between table entries.
static double doc_r_at(double t)
{
    if (t <= DOC_T0) return k_doc_r[0];
    if (t >= DOC_T0 + DOC_N - 1) return k_doc_r[DOC_N - 1];
    int i = (int)floor(t - DOC_T0);
    double f = (t - DOC_T0) - i;
    return k_doc_r[i] + f * (k_doc_r[i + 1] - k_doc_r[i]);
}

// Drive one ADC channel to the voltage that a thermistor at t_c gives.
static void adc_set_temp(int adc_ch, double t_c)
{
    double r = doc_r_at(t_c);
    double v = 3.3 * r / (env->rref_kohm + r);
    int mv = (int)lround(v * 1000.0);
    int raw = (int)lround(mv * 4095.0 / 3300.0);
    if (raw <= PB_NTC_RAW_SHORT_MIN) raw = PB_NTC_RAW_SHORT_MIN + 1;
    if (raw > PB_NTC_RAW_OPEN_MAX) raw = PB_NTC_RAW_OPEN_MAX;
    env->adc_mv[adc_ch] = mv;
    env->adc_raw[adc_ch] = raw;
}

static void adc_set_mv(int adc_ch, int mv)
{
    env->adc_mv[adc_ch] = mv;
    env->adc_raw[adc_ch] = 2000;
}

static pb_ntc_status_t rd(pb_ntc_channel_t ch, float *t)
{
    pb_ntc_status_t st = pb_ntc_read(ch, t);
    CHECK(pb_ntc_last_status(ch) == st);
    if (st != PB_NTC_OK) CHECK(isnan(*t));
    return st;
}

static void ntc_boot(int rref)
{
    env->rref_kohm = rref;
    adc_set_temp(ADC_CHANNEL_0, 25.0);
    adc_set_temp(ADC_CHANNEL_1, 25.0);
    CHECK(pb_ntc_init() == ESP_OK);
    CHECK(pb_ntc_rref_kohm() == rref);
    pb_ntc_load_calibration();
    env_check_quiescent();
}

// --- conversion tests --------------------------------------------------------------------

static void conversion_case(int rref)
{
    ntc_boot(rref);
    float t;
    // Each table point reads back as its temperature (1 mV quantization only).
    for (int i = 0; i < DOC_N; ++i) {
        adc_set_temp(ADC_CHANNEL_1, DOC_T0 + i);
        CHECK(rd(PB_NTC_PTC, &t) == PB_NTC_OK);
        CHECKF(fabsf(t - (float)(DOC_T0 + i)) <= 0.25f, "rref %d: %d C reads %.3f", rref, DOC_T0 + i, (double)t);
    }
    // Sweep every millivolt: monotonic, inside [12, 125], and within 1 C of the
    // stock nearest-entry result (NTC_CONVERSION.md, section 3).
    float prev = 1000.0f;
    for (int mv = 1; mv < 3300; ++mv) {
        adc_set_mv(ADC_CHANNEL_0, mv);
        CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_OK);
        CHECKF(t <= prev + 1e-4f, "rref %d: not monotonic at %d mV", rref, mv);
        CHECK(t >= 12.0f && t <= 125.0f);
        double near_c = doc_nearest_c(doc_r_kohm(mv, rref));
        CHECKF(fabs(t - near_c) < 1.0, "rref %d: %d mV reads %.3f, stock %.0f", rref, mv, (double)t, near_c);
        prev = t;
    }
    // Clamps: colder than the table reads 12 C, hotter reads 125 C.
    adc_set_mv(ADC_CHANNEL_0, 3290);
    CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_OK && t == 12.0f);
    adc_set_mv(ADC_CHANNEL_0, 1);
    CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_OK && t == 125.0f);
}

static void t_conversion_82k(void) { conversion_case(82); }
static void t_conversion_33k(void) { conversion_case(33); }

static void t_rref_select(void)
{
    // The same pin voltage reads hotter with the 33 kOhm divider (lower Rntc).
    float t82, t33;
    ntc_boot(82);
    adc_set_mv(ADC_CHANNEL_0, 1650);
    CHECK(rd(PB_NTC_CHAMBER, &t82) == PB_NTC_OK);
    CHECK(fabsf(t82 - 31.6f) < 0.1f);          // Rntc = Rref at Vsupply/2: 82 kOhm -> 31.6 C
    env->rref_kohm = 33;
    CHECK(pb_ntc_init() == ESP_OK);
    CHECK(rd(PB_NTC_CHAMBER, &t33) == PB_NTC_OK);
    CHECK(fabsf(t33 - 54.17f) < 0.1f);         // 33 kOhm -> 54.17 C
}

static void t_open_short_limits(void)
{
    ntc_boot(82);
    float t;
    env->adc_mv[ADC_CHANNEL_0] = 1000;
    env->adc_raw[ADC_CHANNEL_0] = PB_NTC_RAW_OPEN_MAX + 1;
    CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_OPEN);
    env->adc_raw[ADC_CHANNEL_0] = PB_NTC_RAW_OPEN_MAX;
    CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_OK);
    env->adc_raw[ADC_CHANNEL_0] = PB_NTC_RAW_SHORT_MIN;
    CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_SHORT);
    env->adc_raw[ADC_CHANNEL_0] = 0;
    CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_SHORT);
    env->adc_raw[ADC_CHANNEL_0] = PB_NTC_RAW_SHORT_MIN + 1;
    CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_OK);
    // Rail checks on the calibrated voltage.
    env->adc_raw[ADC_CHANNEL_0] = 2000;
    env->adc_mv[ADC_CHANNEL_0] = 0;
    CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_OPEN);
    env->adc_mv[ADC_CHANNEL_0] = 3300;
    CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_OPEN);
    env->adc_mv[ADC_CHANNEL_0] = 3299;
    CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_OK);
    // Driver errors are UNINIT.
    env->adc_read_err[ADC_CHANNEL_0] = ESP_FAIL;
    CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_UNINIT);
    env->adc_read_err[ADC_CHANNEL_0] = 0;
    env->adc_cali_err = ESP_FAIL;
    CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_UNINIT);
    env->adc_cali_err = 0;
    CHECK(pb_ntc_read((pb_ntc_channel_t)7, &t) == PB_NTC_UNINIT && isnan(t));
}

static void t_uninit_and_init_errors(void)
{
    float t;
    CHECK(pb_ntc_read(PB_NTC_CHAMBER, &t) == PB_NTC_UNINIT && isnan(t));
    CHECK(pb_ntc_last_status(PB_NTC_PTC) == PB_NTC_UNINIT);
    CHECK(isnan(pb_ntc_smoothed_c(PB_NTC_PTC)));
    CHECK(pb_ntc_rref_kohm() == 0);
    env->adc_new_unit_err = ESP_FAIL;
    CHECK(pb_ntc_init() == ESP_FAIL);
    env->adc_new_unit_err = 0;
    env->adc_config_err = ESP_ERR_INVALID_ARG;
    CHECK(pb_ntc_init() == ESP_ERR_INVALID_ARG);
    env->adc_config_err = 0;
    env->adc_cali_create_err = ESP_FAIL;
    CHECK(pb_ntc_init() == ESP_FAIL);
    CHECK(pb_ntc_read(PB_NTC_PTC, &t) == PB_NTC_UNINIT);
}

static void t_smoothing(void)
{
    ntc_boot(82);
    float t;
    CHECK(isnan(pb_ntc_smoothed_c(PB_NTC_CHAMBER)));
    float sum = 0.0f;
    for (int i = 0; i < 5; ++i) {
        adc_set_temp(ADC_CHANNEL_0, 30.0 + 2.0 * i);
        CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_OK);
        sum += t;
    }
    CHECK(fabsf(pb_ntc_smoothed_c(PB_NTC_CHAMBER) - sum / 5.0f) < 1e-4f);
    // A bad sample does not enter the average.
    env->adc_raw[ADC_CHANNEL_0] = 0;
    CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_SHORT);
    CHECK(fabsf(pb_ntc_smoothed_c(PB_NTC_CHAMBER) - sum / 5.0f) < 1e-4f);
}

static void t_swap_thermistors(void)
{
    env_nvs_write("ntc_swap", HOST_NVS_U8, 1);
    env_nvs_write("ntc_off_ch", HOST_NVS_I32, 200);     // +2 C on the logical chamber
    ntc_boot(82);
    CHECK(pb_ntc_get_swap_thermistors() && pb_ntc_get_swap_thermistors_active());
    float t;
    adc_set_temp(ADC_CHANNEL_0, 90.0);                  // socket of the chamber
    adc_set_temp(ADC_CHANNEL_1, 40.0);                  // socket of the PTC
    CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_OK && fabsf(t - 42.0f) < 0.3f);
    CHECK(rd(PB_NTC_PTC, &t) == PB_NTC_OK && fabsf(t - 90.0f) < 0.3f);
    // A fault on a socket follows the logical channel.
    env->adc_raw[ADC_CHANNEL_1] = 0xFFF;
    CHECK(rd(PB_NTC_CHAMBER, &t) == PB_NTC_OPEN);
    CHECK(rd(PB_NTC_PTC, &t) == PB_NTC_OK);
    // A new selection persists, but the live mapping holds until the reboot.
    CHECK(pb_ntc_set_swap_thermistors(false) == ESP_OK);
    CHECK(!pb_ntc_get_swap_thermistors() && pb_ntc_get_swap_thermistors_active());
    uint32_t v;
    CHECK(env_nvs_read("ntc_swap", &v) && v == 0);
    CHECK(rd(PB_NTC_PTC, &t) == PB_NTC_OK && fabsf(t - 90.0f) < 0.3f);
    env->nvs_fail_open_rw = ESP_FAIL;
    CHECK(pb_ntc_set_swap_thermistors(true) == ESP_FAIL);
    env->nvs_fail_open_rw = 0;
    env->nvs_fail_set = ESP_FAIL;
    CHECK(pb_ntc_set_swap_thermistors(true) == ESP_FAIL);
    env->nvs_fail_set = 0;
    CHECK(!pb_ntc_get_swap_thermistors());
    pb_ntc_load_calibration();                          // the next boot
    CHECK(!pb_ntc_get_swap_thermistors_active());
    adc_set_temp(ADC_CHANNEL_1, 40.0);
    CHECK(rd(PB_NTC_PTC, &t) == PB_NTC_OK && fabsf(t - 40.0f) < 0.3f);
}

// --- chain: pb_ntc -> pb_heater ------------------------------------------------------------

static void heater_tick(void)
{
    env->ctx = CTX_CONTROL;
    pb_heater_tick();
    env->ctx = CTX_OTHER;
    env_check_quiescent();
}

static void chain_boot(int rref)
{
    env->ctx = CTX_BOOT;
    CHECK(pb_heater_init() == ESP_OK);
    ntc_boot(rref);
    pb_heater_load_config();
    pb_heater_load_fault();
    env->ctx = CTX_OTHER;
}

// Raise the true temperature of one socket in 0.1 C steps from `from` and
// return the true temperature at which the heater latches `code`.
static double trip_point(int adc_ch, double from, pb_fault_reason_t code)
{
    for (double t = from; t < 140.0; t += 0.1) {
        adc_set_temp(adc_ch, t);
        heater_tick();
        if (pb_heater_is_faulted()) {
            CHECKF(pb_heater_fault_code() == code, "code %d, expected %d",
                   (int)pb_heater_fault_code(), (int)code);
            CHECK(env->level[PB_GPIO_RELAY] == 0);
            return t;
        }
    }
    env_fail(__FILE__, __LINE__, "no trip up to 140 C");
}

static void t_swap_cutoffs_follow(void)
{
    // A 95 C reading on the chamber socket is a chamber over-temp without the
    // swap and only a hot element (no trip) with the swap.
    env_nvs_write("ntc_swap", HOST_NVS_U8, 1);
    chain_boot(82);
    adc_set_temp(ADC_CHANNEL_0, 95.0);
    adc_set_temp(ADC_CHANNEL_1, 40.0);
    heater_tick();
    CHECK(!pb_heater_is_faulted());
    adc_set_temp(ADC_CHANNEL_0, 106.0);
    heater_tick();
    CHECK(pb_heater_is_faulted() && pb_heater_fault_code() == PB_FAULT_PTC_OVERTEMP);
}

static void t_no_swap_cutoffs(void)
{
    chain_boot(82);
    adc_set_temp(ADC_CHANNEL_0, 95.0);
    adc_set_temp(ADC_CHANNEL_1, 40.0);
    heater_tick();
    CHECK(pb_heater_is_faulted() && pb_heater_fault_code() == PB_FAULT_CHAMBER_OVERTEMP);
}

static void cal_case(int rref, float ptc_off, float ch_off, bool via_nvs)
{
    if (via_nvs) {
        // Corrupt stored values: clamp on load.
        env_nvs_write("ntc_off_ptc", HOST_NVS_I32, (uint32_t)(int32_t)(ptc_off * 100.0f));
        env_nvs_write("ntc_off_ch", HOST_NVS_I32, (uint32_t)(int32_t)(ch_off * 100.0f));
    }
    chain_boot(rref);
    if (!via_nvs) {
        CHECK(pb_ntc_set_offset_c(PB_NTC_PTC, ptc_off) == ESP_OK);
        CHECK(pb_ntc_set_offset_c(PB_NTC_CHAMBER, ch_off) == ESP_OK);
    }
    float po = pb_ntc_get_offset_c(PB_NTC_PTC), co = pb_ntc_get_offset_c(PB_NTC_CHAMBER);
    CHECK(fabsf(po) <= PB_NTC_OFFSET_MAX_C && fabsf(co) <= PB_NTC_OFFSET_MAX_C);
    CHECK(po == pb_ntc_clamp_offset_c(ptc_off) && co == pb_ntc_clamp_offset_c(ch_off));

    // PTC: the real trip temperature is 105 C - offset, so in [100, 110] C.
    adc_set_temp(ADC_CHANNEL_0, 30.0);
    double tp = trip_point(ADC_CHANNEL_1, 90.0, PB_FAULT_PTC_OVERTEMP);
    CHECKF(tp >= 100.0 - 0.3 && tp <= 110.0 + 0.3 && fabs(tp - (105.0 - po)) <= 0.3,
           "PTC trips at a real %.2f C with offset %.2f", tp, (double)po);
    CHECK(pb_heater_clear_fault() == ESP_OK);

    // Chamber: the real trip temperature is 85 C - offset, so in [80, 90] C.
    adc_set_temp(ADC_CHANNEL_1, 40.0);
    double tc = trip_point(ADC_CHANNEL_0, 70.0, PB_FAULT_CHAMBER_OVERTEMP);
    CHECKF(tc >= 80.0 - 0.3 && tc <= 90.0 + 0.3 && fabs(tc - (85.0 - co)) <= 0.3,
           "chamber trips at a real %.2f C with offset %.2f", tc, (double)co);
    CHECK(pb_heater_clear_fault() == ESP_OK);

    // An offset cannot hide an open or shorted sensor while heating.
    adc_set_temp(ADC_CHANNEL_0, 30.0);
    adc_set_temp(ADC_CHANNEL_1, 40.0);
    pb_heater_notify_link_alive();
    CHECK(pb_heater_set_target_c(50.0f) == ESP_OK);
    heater_tick();
    CHECK(env->level[PB_GPIO_RELAY] == 1);
    env->adc_raw[ADC_CHANNEL_1] = 0;
    heater_tick();
    CHECK(env->level[PB_GPIO_RELAY] == 0);
    CHECK(pb_heater_fault_code() == PB_FAULT_PTC_SENSOR);
}

static void t_cal_set_max_negative_82k(void)  { cal_case(82, -1000.0f, -1000.0f, false); }
static void t_cal_set_max_positive_33k(void)  { cal_case(33, 1000.0f, 1000.0f, false); }
static void t_cal_set_nan(void)               { cal_case(82, NAN, -INFINITY, false); }
static void t_cal_nvs_negative(void)          { cal_case(82, -1.0e6f, -1.0e6f, true); }
static void t_cal_nvs_positive_33k(void)      { cal_case(33, 1.0e6f, 3.0f, true); }

static void t_cal_set_persist_first(void)
{
    ntc_boot(82);
    CHECK(pb_ntc_set_offset_c(PB_NTC_PTC, 2.0f) == ESP_OK);
    uint32_t v;
    CHECK(env_nvs_read("ntc_off_ptc", &v) && (int32_t)v == 200);
    env->nvs_fail_open_rw = ESP_FAIL;
    CHECK(pb_ntc_set_offset_c(PB_NTC_PTC, -3.0f) == ESP_FAIL);
    CHECK(pb_ntc_get_offset_c(PB_NTC_PTC) == 2.0f);
    env->nvs_fail_open_rw = 0;
    env->nvs_fail_set = ESP_FAIL;
    CHECK(pb_ntc_set_offset_c(PB_NTC_PTC, -3.0f) == ESP_FAIL);
    CHECK(pb_ntc_get_offset_c(PB_NTC_PTC) == 2.0f);     // RAM unchanged
    CHECK(env_nvs_read("ntc_off_ptc", &v) && (int32_t)v == 200);
    env->nvs_fail_set = 0;
    CHECK(pb_ntc_set_offset_c((pb_ntc_channel_t)5, 1.0f) == ESP_ERR_INVALID_ARG);
    CHECK(pb_ntc_get_offset_c((pb_ntc_channel_t)5) == 0.0f);
    env_check_quiescent();
}

int main(void)
{
    env_init();
    bool ok = true;
#define RUN(fn) do { env_reset(); ok &= env_run(#fn, fn); } while (0)
    RUN(t_conversion_82k);
    RUN(t_conversion_33k);
    RUN(t_rref_select);
    RUN(t_open_short_limits);
    RUN(t_uninit_and_init_errors);
    RUN(t_smoothing);
    RUN(t_swap_thermistors);
    RUN(t_swap_cutoffs_follow);
    RUN(t_no_swap_cutoffs);
    RUN(t_cal_set_max_negative_82k);
    RUN(t_cal_set_max_positive_33k);
    RUN(t_cal_set_nan);
    RUN(t_cal_nvs_negative);
    RUN(t_cal_nvs_positive_33k);
    RUN(t_cal_set_persist_first);
    if (!ok) {
        fprintf(stderr, "pb_ntc chain host test: FAILED\n");
        return 1;
    }
    printf("pb_ntc chain host test: all passed\n");
    return 0;
}
