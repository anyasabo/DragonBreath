// SPDX-License-Identifier: MIT
// Host test of the first-heat check in a report-only build of the REAL
// pb_heater.c: a CONFIG_PB_DEVBOARD_SAFE build (no mains, no element), or a
// production build with CONFIG_PB_FIRST_HEAT_REPORT_ONLY=y (the recovery for a false
// trip). The check must report its result and never latch or store a result.
#define _DEFAULT_SOURCE
#include "heater_host_env.h"
#include "pb_board.h"
#include "pb_heater.h"
#include "pb_ntc.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#if PB_FH_ENFORCED
#error "build this test with -DCONFIG_PB_DEVBOARD_SAFE or -DCONFIG_PB_FIRST_HEAT_REPORT_ONLY"
#endif

pb_ntc_status_t pb_ntc_read(pb_ntc_channel_t ch, float *out_c)
{
    pb_ntc_status_t st = (pb_ntc_status_t)env->ntc_status[ch];
    if (out_c) *out_c = (st == PB_NTC_OK) ? env->ntc_temp[ch] : NAN;
    return st;
}
int pb_ntc_rref_kohm(void) { return 82; }
bool pb_ntc_get_swap_thermistors_active(void) { return false; }
float pb_ntc_get_offset_c(pb_ntc_channel_t ch) { (void)ch; return 0.0f; }

static void tick(void)
{
    env->ctx = CTX_CONTROL;
    pb_heater_tick();
    env->ctx = CTX_OTHER;
    env_check_quiescent();
#ifdef CONFIG_PB_DEVBOARD_SAFE
    CHECK(env->writes[PB_GPIO_RELAY] == 0);     // the relay GPIO is compiled out
#else
    CHECK(env->level[PB_GPIO_RELAY] == (pb_heater_is_on() ? 1 : 0));
#endif
}

static void heat(float dch, float dptc, int ticks)
{
    for (int i = 0; i < ticks; ++i) {
        env->now_us += 500000;
        env->ntc_temp[PB_NTC_CHAMBER] += dch;
        env->ntc_temp[PB_NTC_PTC] += dptc;
        pb_heater_notify_link_alive();
        tick();
    }
}

static pb_heater_first_heat_t fh(void)
{
    pb_heater_first_heat_t f;
    pb_heater_get_first_heat(&f);
    return f;
}

static void boot_and_start(void)
{
    env->ctx = CTX_BOOT;
    CHECK(pb_heater_init() == ESP_OK);
    pb_heater_load_config();
    pb_heater_load_fault();
    pb_heater_load_first_heat();
    env->ctx = CTX_OTHER;
    CHECK(!fh().enforced);
    pb_heater_notify_link_alive();
    CHECK(pb_heater_set_target_c(70.0f) == ESP_OK);
    tick();
    CHECK(pb_heater_is_on() && fh().state == PB_FH_ARMED);
}

static void t_dev_no_rise_reports_only(void)
{
    boot_and_start();
    heat(0.0f, 0.0f, 80);                       // 40 s ON, no rise
    CHECK(fh().state == PB_FH_NO_RISE);
    CHECK(!pb_heater_is_faulted() && pb_heater_is_on() && pb_heater_heat_mode());
    CHECK(env->nvs_sets == 0);                  // no result and no latch stored
    // The report stays until the heat stops. Then the next heat checks again.
    CHECK(pb_heater_set_target_c(0.0f) == ESP_OK);
    tick();
    CHECK(fh().state == PB_FH_IDLE);
    CHECK(pb_heater_set_target_c(70.0f) == ESP_OK);
    tick();
    CHECK(fh().state == PB_FH_ARMED);
}

static void t_dev_crossed_reports_only(void)
{
    boot_and_start();
    heat(0.5f, 0.0f, 30);                       // 15 s ON, chamber 40 C < target
    CHECK(fh().state == PB_FH_WRONG_ROLES);
    CHECK(!pb_heater_is_faulted() && pb_heater_is_on());
    CHECK(env->nvs_sets == 0);
}

static void t_dev_pass_not_stored(void)
{
    boot_and_start();
    heat(0.1f, 1.0f, 30);
    CHECK(fh().state == PB_FH_PASS && fh().stored == PB_FH_STORED_UNKNOWN);
    CHECK(env->nvs_sets == 0);
}

static void t_dev_hazard_still_trips(void)
{
    // The hazard trips stay active on a dev board.
    boot_and_start();
    env->ntc_temp[PB_NTC_PTC] = PB_HEATER_PTC_CUTOFF_C;
    tick();
    CHECK(pb_heater_is_faulted() && pb_heater_fault_code() == PB_FAULT_PTC_OVERTEMP);
    CHECK(!pb_heater_is_on());
}

int main(void)
{
    env_init();
    bool ok = true;
#define RUN(fn) do { env_reset(); ok &= env_run(#fn, fn); } while (0)
    RUN(t_dev_no_rise_reports_only);
    RUN(t_dev_crossed_reports_only);
    RUN(t_dev_pass_not_stored);
    RUN(t_dev_hazard_still_trips);
    if (!ok) {
        fprintf(stderr, "pb_heater first-heat report-only host test: FAILED\n");
        return 1;
    }
#ifdef CONFIG_PB_DEVBOARD_SAFE
    printf("pb_heater first-heat report-only host test (dev board): all passed\n");
#else
    printf("pb_heater first-heat report-only host test (REPORT_ONLY=y): all passed\n");
#endif
    return 0;
}
