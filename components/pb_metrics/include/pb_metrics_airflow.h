// SPDX-License-Identifier: MIT
// pb_metrics_airflow — report-only airflow signs, computed in the control task.
//
// 1. Rise ratio: at each heat start, the outlet (chamber NTC) rise against the
//    PTC rise over the first PB_AIRFLOW_RISE_WINDOW_MS of relay on-time, in
//    permille. Good airflow moves heat from the element to the outlet. A
//    stalled fan gives a fast PTC rise and a slow outlet rise: a low ratio.
// 2. PTC cooldown time constant with the fan on: after the relay goes off, the
//    time until the PTC excess over the chamber falls to 1/e (36.8 %) of the
//    start excess. Less airflow gives a longer time.
// The foldback count is in the pb_heater counters.
//
// Integer math only, O(1) for each tick, no logging. The limits are not known
// yet: the values are for a baseline from the first good heats.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PB_AIRFLOW_UNKNOWN            INT32_MIN
#define PB_AIRFLOW_RISE_WINDOW_MS     45000   // relay on-time (30-60 s band)
#define PB_AIRFLOW_RISE_MIN_PTC_DC    50      // PTC rise below 5.0 C: no sample
#define PB_AIRFLOW_COOL_MIN_EXCESS_DC 150     // start only with 15.0 C or more
#define PB_AIRFLOW_COOL_MAX_MS        (30u * 60u * 1000u)
#define PB_AIRFLOW_MAX_STEP_MS        2000    // a longer gap counts as 2 s
#define PB_AIRFLOW_INV_E_PERMILLE     368     // 1/e

typedef struct {
    // Inputs of the previous tick.
    bool started;
    int64_t prev_us;
    bool prev_heat;
    bool prev_relay;

    // Rise ratio in progress.
    bool rise_active;
    int32_t rise_ptc0_dc;
    int32_t rise_ch0_dc;
    uint32_t rise_on_ms;

    // Cooldown in progress.
    bool cool_active;
    int32_t cool_excess0_dc;
    uint32_t cool_ms;

    // Results.
    int32_t rise_ratio_permille;    // last value, PB_AIRFLOW_UNKNOWN before the first
    uint32_t rise_samples;
    uint32_t cool_tau_ms;           // last value, 0 before the first
    uint32_t cool_samples;
} pb_airflow_t;

void pb_airflow_init(pb_airflow_t *a);

// One control tick. heat: a target is armed (pb_heater_heat_mode). relay: the
// SSR output. fan_on: the blower runs. Temperatures in tenths of a degree C, or
// PB_AIRFLOW_UNKNOWN if the sensor status is not ok.
void pb_airflow_update(pb_airflow_t *a, int64_t now_us, bool heat, bool relay,
                       bool fan_on, int32_t ptc_dc, int32_t chamber_dc);
