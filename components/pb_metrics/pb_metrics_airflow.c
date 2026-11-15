// SPDX-License-Identifier: MIT
// pb_metrics_airflow — see pb_metrics_airflow.h.
#include "pb_metrics_airflow.h"

#include <string.h>

void pb_airflow_init(pb_airflow_t *a)
{
    if (!a) return;
    memset(a, 0, sizeof *a);
    a->rise_ratio_permille = PB_AIRFLOW_UNKNOWN;
}

static uint32_t step_ms(const pb_airflow_t *a, int64_t now_us)
{
    if (!a->started || now_us <= a->prev_us) return 0;
    int64_t d = (now_us - a->prev_us) / 1000;
    return d > PB_AIRFLOW_MAX_STEP_MS ? PB_AIRFLOW_MAX_STEP_MS : (uint32_t)d;
}

void pb_airflow_update(pb_airflow_t *a, int64_t now_us, bool heat, bool relay,
                       bool fan_on, int32_t ptc_dc, int32_t chamber_dc)
{
    if (!a) return;
    const bool temps_ok = ptc_dc != PB_AIRFLOW_UNKNOWN && chamber_dc != PB_AIRFLOW_UNKNOWN;
    const uint32_t dt = step_ms(a, now_us);

    // Rise ratio. The on-time of the last interval counts if the relay was on
    // at its start.
    if (a->rise_active) {
        if (!heat || !temps_ok) {
            a->rise_active = false;            // heat stopped or a sensor failed
        } else {
            if (a->prev_relay) a->rise_on_ms += dt;
            if (a->rise_on_ms >= PB_AIRFLOW_RISE_WINDOW_MS) {
                int32_t dptc = ptc_dc - a->rise_ptc0_dc;
                int32_t dch = chamber_dc - a->rise_ch0_dc;
                if (dptc >= PB_AIRFLOW_RISE_MIN_PTC_DC) {
                    a->rise_ratio_permille = (int32_t)(((int64_t)dch * 1000) / dptc);
                    a->rise_samples++;
                }
                a->rise_active = false;
            }
        }
    }
    if (heat && (!a->started || !a->prev_heat) && temps_ok) {
        a->rise_active = true;                 // a heat start
        a->rise_ptc0_dc = ptc_dc;
        a->rise_ch0_dc = chamber_dc;
        a->rise_on_ms = 0;
    }

    // Cooldown time constant.
    if (a->cool_active) {
        if (relay || !fan_on || !temps_ok) {
            a->cool_active = false;
        } else {
            a->cool_ms += dt;
            int64_t excess = (int64_t)ptc_dc - chamber_dc;
            if (excess * 1000 <= (int64_t)a->cool_excess0_dc * PB_AIRFLOW_INV_E_PERMILLE) {
                a->cool_tau_ms = a->cool_ms;
                a->cool_samples++;
                a->cool_active = false;
            } else if (a->cool_ms > PB_AIRFLOW_COOL_MAX_MS) {
                a->cool_active = false;
            }
        }
    }
    if (a->started && a->prev_relay && !relay && fan_on && temps_ok
            && ptc_dc - chamber_dc >= PB_AIRFLOW_COOL_MIN_EXCESS_DC) {
        a->cool_active = true;                 // the relay went off
        a->cool_excess0_dc = ptc_dc - chamber_dc;
        a->cool_ms = 0;
    }

    a->started = true;
    a->prev_us = now_us;
    a->prev_heat = heat;
    a->prev_relay = relay;
}
