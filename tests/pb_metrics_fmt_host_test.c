// SPDX-License-Identifier: MIT
// Host test for the pure Prometheus text writer (components/pb_metrics).
#include "pb_metrics_fmt.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char out[PB_METRICS_BUF_SIZE];

static void known_snapshot(pb_metrics_snapshot_t *s)
{
    memset(s, 0, sizeof *s);
    s->chamber_dc = 362;
    s->ptc_dc = 731;
    s->chamber_status = 0;
    s->ptc_status = 0;
    s->heater_demand = true;
    s->relay_on = true;
    s->fan_percent = 100;
    s->mode = 2;                       // auto
    s->target_dc = 450;
    s->zone_target_dc = 500;
    s->fault_code = 0;
    s->source_connected = true;
    s->trips[1] = 1;                   // ptc_overtemp
    s->trips[5] = 2;                   // link_lost
    s->trips[6] = 9;                   // panic_off: not in the trips family
    s->panic_offs = 3;
    s->relay_on_ms = 123456;
    s->ticks = 7200;
    s->ticks_late = 1;
    s->tick_last_us = 850;
    s->tick_max_us = 41000;
    s->tick_interval_max_us = 512000;
    s->heap_free = 61000;
    s->heap_min_free = 42000;
    s->heap_largest_block = 30000;
    s->task_count = 3;
    s->n_tasks = 2;
    strcpy(s->tasks[0].name, "pb_control");
    s->tasks[0].stack_free_min_bytes = 1800;
    s->tasks[0].cpu_us = 1234567;
    strcpy(s->tasks[1].name, "IDLE");
    s->tasks[1].stack_free_min_bytes = 600;
    s->tasks[1].cpu_us = 5;
    s->uptime_ms = 3600005;
    s->boot_count = 42;
    s->reset_reason = 6;               // task_wdt
    s->version = "v1.2.0";
    s->idf_version = "v5.3.6";
    s->rssi_valid = true;
    s->rssi_dbm = -61;
    s->http_sockets = 2;
    s->last_build_us = 1500;
}

static const char k_expected[] =
    "# HELP dragonbreath_temperature_celsius Smoothed sensor temperature. No sample if the sensor status is not ok.\n"
    "# TYPE dragonbreath_temperature_celsius gauge\n"
    "dragonbreath_temperature_celsius{sensor=\"chamber\"} 36.2\n"
    "dragonbreath_temperature_celsius{sensor=\"ptc\"} 73.1\n"
    "# HELP dragonbreath_sensor_status Thermistor status. 1 for the current status, 0 for the others.\n"
    "# TYPE dragonbreath_sensor_status gauge\n"
    "dragonbreath_sensor_status{sensor=\"chamber\",status=\"ok\"} 1\n"
    "dragonbreath_sensor_status{sensor=\"chamber\",status=\"short\"} 0\n"
    "dragonbreath_sensor_status{sensor=\"chamber\",status=\"open\"} 0\n"
    "dragonbreath_sensor_status{sensor=\"chamber\",status=\"uninit\"} 0\n"
    "dragonbreath_sensor_status{sensor=\"ptc\",status=\"ok\"} 1\n"
    "dragonbreath_sensor_status{sensor=\"ptc\",status=\"short\"} 0\n"
    "dragonbreath_sensor_status{sensor=\"ptc\",status=\"open\"} 0\n"
    "dragonbreath_sensor_status{sensor=\"ptc\",status=\"uninit\"} 0\n"
    "# HELP dragonbreath_heater_demand 1 if a target is armed and no fault is latched.\n"
    "# TYPE dragonbreath_heater_demand gauge\n"
    "dragonbreath_heater_demand 1\n"
    "# HELP dragonbreath_heater_relay_on 1 if the SSR output is on.\n"
    "# TYPE dragonbreath_heater_relay_on gauge\n"
    "dragonbreath_heater_relay_on 1\n"
    "# HELP dragonbreath_fan_percent Effective blower level in percent.\n"
    "# TYPE dragonbreath_fan_percent gauge\n"
    "dragonbreath_fan_percent 100\n"
    "# HELP dragonbreath_mode Control mode. 1 for the current mode, 0 for the others.\n"
    "# TYPE dragonbreath_mode gauge\n"
    "dragonbreath_mode{mode=\"off\"} 0\n"
    "dragonbreath_mode{mode=\"power_on\"} 0\n"
    "dragonbreath_mode{mode=\"auto\"} 1\n"
    "dragonbreath_mode{mode=\"drying\"} 0\n"
    "# HELP dragonbreath_target_celsius Effective heater target. 0 when off.\n"
    "# TYPE dragonbreath_target_celsius gauge\n"
    "dragonbreath_target_celsius 45.0\n"
    "# HELP dragonbreath_zone_target_celsius Filament-zone target from the control source. No sample if no zone is known.\n"
    "# TYPE dragonbreath_zone_target_celsius gauge\n"
    "dragonbreath_zone_target_celsius 50.0\n"
    "# HELP dragonbreath_fault_latched 1 if a safety fault or an inhibit is latched.\n"
    "# TYPE dragonbreath_fault_latched gauge\n"
    "dragonbreath_fault_latched 0\n"
    "# HELP dragonbreath_fault_code Cause code of the latched fault (pb_fault_reason_t, 0 = none).\n"
    "# TYPE dragonbreath_fault_code gauge\n"
    "dragonbreath_fault_code 0\n"
    "# HELP dragonbreath_heater_inhibited 1 if heat is inhibited until reboot.\n"
    "# TYPE dragonbreath_heater_inhibited gauge\n"
    "dragonbreath_heater_inhibited 0\n"
    "# HELP dragonbreath_source_connected 1 if the control source link is up.\n"
    "# TYPE dragonbreath_source_connected gauge\n"
    "dragonbreath_source_connected 1\n"
    "# HELP dragonbreath_safety_trips_total New safety latches since boot, by cause. Panic-offs are in dragonbreath_panic_offs_total.\n"
    "# TYPE dragonbreath_safety_trips_total counter\n"
    "dragonbreath_safety_trips_total{cause=\"ptc_overtemp\"} 1\n"
    "dragonbreath_safety_trips_total{cause=\"chamber_overtemp\"} 0\n"
    "dragonbreath_safety_trips_total{cause=\"chamber_sensor\"} 0\n"
    "dragonbreath_safety_trips_total{cause=\"ptc_sensor\"} 0\n"
    "dragonbreath_safety_trips_total{cause=\"link_lost\"} 2\n"
    "dragonbreath_safety_trips_total{cause=\"inhibited\"} 0\n"
    "dragonbreath_safety_trips_total{cause=\"emergency\"} 0\n"
    "# HELP dragonbreath_panic_offs_total Front-panel panic-off requests since boot.\n"
    "# TYPE dragonbreath_panic_offs_total counter\n"
    "dragonbreath_panic_offs_total 3\n"
    "# HELP dragonbreath_relay_on_seconds_total Time with the SSR output on since boot.\n"
    "# TYPE dragonbreath_relay_on_seconds_total counter\n"
    "dragonbreath_relay_on_seconds_total 123.456\n"
    "# HELP dragonbreath_control_ticks_total Control loop iterations since boot.\n"
    "# TYPE dragonbreath_control_ticks_total counter\n"
    "dragonbreath_control_ticks_total 7200\n"
    "# HELP dragonbreath_control_ticks_late_total Periodic control ticks that started more than 250 ms after the scheduled time.\n"
    "# TYPE dragonbreath_control_ticks_late_total counter\n"
    "dragonbreath_control_ticks_late_total 1\n"
    "# HELP dragonbreath_control_tick_duration_microseconds Duration of the last control tick.\n"
    "# TYPE dragonbreath_control_tick_duration_microseconds gauge\n"
    "dragonbreath_control_tick_duration_microseconds 850\n"
    "# HELP dragonbreath_control_tick_duration_max_microseconds Longest control tick since boot.\n"
    "# TYPE dragonbreath_control_tick_duration_max_microseconds gauge\n"
    "dragonbreath_control_tick_duration_max_microseconds 41000\n"
    "# HELP dragonbreath_control_tick_interval_max_microseconds Longest time between the starts of two control ticks since boot.\n"
    "# TYPE dragonbreath_control_tick_interval_max_microseconds gauge\n"
    "dragonbreath_control_tick_interval_max_microseconds 512000\n"
    "# HELP dragonbreath_heap_free_bytes Free heap.\n"
    "# TYPE dragonbreath_heap_free_bytes gauge\n"
    "dragonbreath_heap_free_bytes 61000\n"
    "# HELP dragonbreath_heap_minimum_free_bytes Lowest free heap since boot.\n"
    "# TYPE dragonbreath_heap_minimum_free_bytes gauge\n"
    "dragonbreath_heap_minimum_free_bytes 42000\n"
    "# HELP dragonbreath_heap_largest_free_block_bytes Largest free heap block.\n"
    "# TYPE dragonbreath_heap_largest_free_block_bytes gauge\n"
    "dragonbreath_heap_largest_free_block_bytes 30000\n"
    "# HELP dragonbreath_tasks Number of FreeRTOS tasks.\n"
    "# TYPE dragonbreath_tasks gauge\n"
    "dragonbreath_tasks 3\n"
    "# HELP dragonbreath_task_stack_free_minimum_bytes Stack high-water mark: the least free stack of the task since it started.\n"
    "# TYPE dragonbreath_task_stack_free_minimum_bytes gauge\n"
    "dragonbreath_task_stack_free_minimum_bytes{task=\"pb_control\"} 1800\n"
    "dragonbreath_task_stack_free_minimum_bytes{task=\"IDLE\"} 600\n"
    "# HELP dragonbreath_task_cpu_seconds_total CPU time of the task since it started. rate() gives the CPU share.\n"
    "# TYPE dragonbreath_task_cpu_seconds_total counter\n"
    "dragonbreath_task_cpu_seconds_total{task=\"pb_control\"} 1.234567\n"
    "dragonbreath_task_cpu_seconds_total{task=\"IDLE\"} 0.000005\n"
    "# HELP dragonbreath_uptime_seconds Time since boot.\n"
    "# TYPE dragonbreath_uptime_seconds gauge\n"
    "dragonbreath_uptime_seconds 3600.005\n"
    "# HELP dragonbreath_boots_total Boot count, kept in NVS. A factory reset sets it to 0.\n"
    "# TYPE dragonbreath_boots_total counter\n"
    "dragonbreath_boots_total 42\n"
    "# HELP dragonbreath_reset_reason esp_reset_reason() of this boot (numeric).\n"
    "# TYPE dragonbreath_reset_reason gauge\n"
    "dragonbreath_reset_reason 6\n"
    "# HELP dragonbreath_reset_reason_info esp_reset_reason() of this boot as a label.\n"
    "# TYPE dragonbreath_reset_reason_info gauge\n"
    "dragonbreath_reset_reason_info{reason=\"task_wdt\"} 1\n"
    "# HELP dragonbreath_build_info Firmware version.\n"
    "# TYPE dragonbreath_build_info gauge\n"
    "dragonbreath_build_info{version=\"v1.2.0\",idf_version=\"v5.3.6\"} 1\n"
    "# HELP dragonbreath_wifi_rssi_dbm Wi-Fi RSSI of the station link. No sample if not connected.\n"
    "# TYPE dragonbreath_wifi_rssi_dbm gauge\n"
    "dragonbreath_wifi_rssi_dbm -61\n"
    "# HELP dragonbreath_http_open_sockets Open client sockets of the HTTP server.\n"
    "# TYPE dragonbreath_http_open_sockets gauge\n"
    "dragonbreath_http_open_sockets 2\n"
    "# HELP dragonbreath_metrics_build_duration_microseconds Time to collect and format the previous /metrics response.\n"
    "# TYPE dragonbreath_metrics_build_duration_microseconds gauge\n"
    "dragonbreath_metrics_build_duration_microseconds 1500\n";

static void diff_fail(const char *got, const char *want)
{
    size_t i = 0;
    while (got[i] && got[i] == want[i]) i++;
    size_t from = i > 60 ? i - 60 : 0;
    fprintf(stderr, "first difference at byte %zu\n got: %.120s\nwant: %.120s\n",
            i, got + from, want + from);
    exit(1);
}

static void test_known_snapshot(void)
{
    pb_metrics_snapshot_t s;
    known_snapshot(&s);
    int n = pb_metrics_format(&s, out, sizeof out);
    assert(n > 0);
    assert((size_t)n == strlen(out));
    if (strcmp(out, k_expected) != 0) diff_fail(out, k_expected);
}

static void test_negative_temperatures(void)
{
    static const struct { int32_t dc; const char *text; } cases[] = {
        { -52, "-5.2" }, { -5, "-0.5" }, { -10, "-1.0" }, { 0, "0.0" },
        { 9, "0.9" }, { 1055, "105.5" }, { -2147483647, "-214748364.7" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pb_metrics_snapshot_t s;
        known_snapshot(&s);
        s.chamber_dc = cases[i].dc;
        s.target_dc = cases[i].dc;
        assert(pb_metrics_format(&s, out, sizeof out) > 0);
        char line[96];
        snprintf(line, sizeof line,
                 "\ndragonbreath_temperature_celsius{sensor=\"chamber\"} %s\n", cases[i].text);
        assert(strstr(out, line));
        snprintf(line, sizeof line, "\ndragonbreath_target_celsius %s\n", cases[i].text);
        assert(strstr(out, line));
    }
}

// Choice: an invalid reading has no temperature sample (no NaN in the output).
// dragonbreath_sensor_status tells why. HELP and TYPE stay, so the family is stable.
static void test_unknown_values(void)
{
    pb_metrics_snapshot_t s;
    known_snapshot(&s);
    s.chamber_dc = PB_METRICS_TEMP_UNKNOWN;
    s.chamber_status = 2;              // open
    s.zone_target_dc = PB_METRICS_TEMP_UNKNOWN;
    s.rssi_valid = false;
    s.http_sockets = -1;
    assert(pb_metrics_format(&s, out, sizeof out) > 0);
    assert(!strstr(out, "sensor=\"chamber\"} "));
    assert(strstr(out, "\ndragonbreath_temperature_celsius{sensor=\"ptc\"} 73.1\n"));
    assert(strstr(out, "\ndragonbreath_sensor_status{sensor=\"chamber\",status=\"open\"} 1\n"));
    assert(strstr(out, "\ndragonbreath_sensor_status{sensor=\"chamber\",status=\"ok\"} 0\n"));
    assert(strstr(out, "# TYPE dragonbreath_zone_target_celsius gauge\n# HELP"));
    assert(!strstr(out, "\ndragonbreath_wifi_rssi_dbm "));
    assert(!strstr(out, "\ndragonbreath_http_open_sockets "));
    assert(!strstr(out, "NaN") && !strstr(out, "nan"));
}

static void test_buffer_too_small(void)
{
    pb_metrics_snapshot_t s;
    known_snapshot(&s);
    int n = pb_metrics_format(&s, out, sizeof out);
    assert(n > 0);
    char *small = malloc((size_t)n + 1);
    assert(small);
    // The text plus its NUL needs n + 1 bytes. Each smaller buffer must fail.
    for (size_t cap = 0; cap <= (size_t)n; cap++)
        assert(pb_metrics_format(&s, small, cap) == -1);
    assert(pb_metrics_format(&s, small, (size_t)n + 1) == n);
    assert(strcmp(small, out) == 0);
    free(small);
    assert(pb_metrics_format(NULL, out, sizeof out) == -1);
    assert(pb_metrics_format(&s, NULL, sizeof out) == -1);
}

static void test_label_escaping(void)
{
    pb_metrics_snapshot_t s;
    known_snapshot(&s);
    strcpy(s.tasks[0].name, "a\"b\\c\nd");
    s.version = "1.0\"x";
    s.idf_version = NULL;
    assert(pb_metrics_format(&s, out, sizeof out) > 0);
    assert(strstr(out, "{task=\"a\\\"b\\\\c\\nd\"} 1800\n"));
    assert(strstr(out, "{version=\"1.0\\\"x\",idf_version=\"\"} 1\n"));
}

static void test_state_sets_and_ranges(void)
{
    pb_metrics_snapshot_t s;
    known_snapshot(&s);
    s.mode = 9;                        // out of range: every mode is 0
    s.reset_reason = 200;
    s.n_tasks = 60;                    // capped at PB_METRICS_MAX_TASKS
    assert(pb_metrics_format(&s, out, sizeof out) > 0);
    assert(strstr(out, "\ndragonbreath_mode{mode=\"off\"} 0\n"));
    assert(strstr(out, "\ndragonbreath_mode{mode=\"auto\"} 0\n"));
    assert(strstr(out, "dragonbreath_reset_reason_info{reason=\"unknown\"} 1\n"));
    assert(strcmp(pb_metrics_reset_reason_str(1), "poweron") == 0);
    assert(strcmp(pb_metrics_reset_reason_str(15), "cpu_lockup") == 0);
    assert(strcmp(pb_metrics_reset_reason_str(16), "unknown") == 0);
}

// Worst case on the device: all task slots used, 15-character FreeRTOS names
// with a "#NN" duplicate suffix, 31-character version strings (the app
// descriptor fields are char[32]) and maximum values. It must fit in
// PB_METRICS_BUF_SIZE with at least 25 % margin.
static void worst_case_snapshot(pb_metrics_snapshot_t *s)
{
    known_snapshot(s);
    s->chamber_dc = -2147483647;
    s->ptc_dc = -2147483647;
    s->target_dc = -2147483647;
    s->zone_target_dc = -2147483647;
    for (int i = 0; i < PB_METRICS_FAULT_CAUSES; i++) s->trips[i] = UINT32_MAX;
    s->panic_offs = s->ticks = s->ticks_late = UINT32_MAX;
    s->relay_on_ms = s->uptime_ms = UINT64_MAX;
    s->tick_last_us = s->tick_max_us = s->tick_interval_max_us = UINT32_MAX;
    s->heap_free = s->heap_min_free = s->heap_largest_block = UINT32_MAX;
    s->task_count = UINT16_MAX;
    s->n_tasks = PB_METRICS_MAX_TASKS;
    for (int i = 0; i < PB_METRICS_MAX_TASKS; i++) {
        snprintf(s->tasks[i].name, sizeof s->tasks[i].name, "abcdefghijklmno#%d", i + 10);
        s->tasks[i].stack_free_min_bytes = UINT32_MAX;
        s->tasks[i].cpu_us = UINT64_MAX;
    }
    s->boot_count = UINT32_MAX;
    s->version = "v1.2.3-45-gabcdef0-dirty-abcdef";
    s->idf_version = "v5.3.6-dirty-abcdefghijklmnopqr";
    s->rssi_dbm = -128;
    s->http_sockets = INT16_MAX;
    s->last_build_us = UINT32_MAX;
}

static void test_worst_case_size(void)
{
    pb_metrics_snapshot_t s;
    worst_case_snapshot(&s);
    assert(strlen(s.version) == 31 && strlen(s.idf_version) == 31);
    int n = pb_metrics_format(&s, out, sizeof out);
    assert(n > 0);
    printf("worst-case /metrics size: %d bytes (buffer %d)\n", n, PB_METRICS_BUF_SIZE);
    assert(n * 4 <= PB_METRICS_BUF_SIZE * 3);

    // Names that need escaping at each byte double in size. The writer must
    // still stop at the buffer end: it returns -1 or a complete text.
    for (int i = 0; i < PB_METRICS_MAX_TASKS; i++) {
        memset(s.tasks[i].name, '\\', PB_METRICS_TASK_NAME_LEN);
        s.tasks[i].name[PB_METRICS_TASK_NAME_LEN] = '\0';
    }
    n = pb_metrics_format(&s, out, sizeof out);
    assert(n == -1 || (n > 0 && (size_t)n == strlen(out)));
}

int main(void)
{
    test_known_snapshot();
    test_negative_temperatures();
    test_unknown_values();
    test_buffer_too_small();
    test_label_escaping();
    test_state_sets_and_ranges();
    test_worst_case_size();
    printf("pb_metrics_fmt host test: PASS\n");
    return 0;
}
