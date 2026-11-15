// SPDX-License-Identifier: MIT
// Host test for the pure parts of GET /metrics (components/pb_metrics): the
// streaming Prometheus writer, task aggregation, tick statistics and the
// temperature conversion. CHECK() works with -DNDEBUG too.
//
// "--dump" writes the text of a realistic snapshot to stdout, for the
// prometheus-client parse check in run_metrics_fmt_host_test.sh.
#include "pb_metrics_fmt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

// Sink that appends to a test buffer. fail_after < 0: never fail.
typedef struct {
    char data[32768];
    size_t len;
    int calls;
    int fail_after;
    size_t max_piece;
} capture_t;

static bool capture_sink(void *ctx, const char *data, size_t len)
{
    capture_t *c = ctx;
    if (c->fail_after >= 0 && c->calls >= c->fail_after) return false;
    c->calls++;
    if (len > c->max_piece) c->max_piece = len;
    if (len > sizeof c->data - 1 - c->len) return false;
    memcpy(c->data + c->len, data, len);
    c->len += len;
    c->data[c->len] = '\0';
    return true;
}

static capture_t cap;

// Format s through a chunk buffer of chunk_size bytes into cap.data.
static long format_with(const pb_metrics_snapshot_t *s, size_t chunk_size)
{
    memset(&cap, 0, sizeof cap);
    cap.fail_after = -1;
    char *chunk = malloc(chunk_size);
    CHECK(chunk != NULL);
    long n = pb_metrics_format(s, chunk, chunk_size, capture_sink, &cap);
    free(chunk);
    return n;
}

static long format(const pb_metrics_snapshot_t *s)
{
    return format_with(s, 1024);
}

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
    s->task_stack_valid = true;
    s->task_cpu_valid = true;
    s->task_count = 3;
    // Two tasks share the name IDLE: one series, CPU summed, stack minimum.
    CHECK(pb_metrics_add_task(s, "pb_control", 1800, 1234567));
    CHECK(pb_metrics_add_task(s, "IDLE", 600, 5));
    CHECK(pb_metrics_add_task(s, "IDLE", 500, 1000000));
    s->uptime_ms = 3600005;
    s->boot_count = 42;
    s->reset_reason = 6;               // task_wdt
    s->version = "v1.2.0";
    s->idf_version = "v5.3.6";
    s->rssi_valid = true;
    s->rssi_dbm = -61;
    s->http_sockets = 2;
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
    "# HELP dragonbreath_fault Cause of the latched fault. 1 for the current cause (none if no latch), 0 for the others.\n"
    "# TYPE dragonbreath_fault gauge\n"
    "dragonbreath_fault{cause=\"none\"} 1\n"
    "dragonbreath_fault{cause=\"ptc_overtemp\"} 0\n"
    "dragonbreath_fault{cause=\"chamber_overtemp\"} 0\n"
    "dragonbreath_fault{cause=\"chamber_sensor\"} 0\n"
    "dragonbreath_fault{cause=\"ptc_sensor\"} 0\n"
    "dragonbreath_fault{cause=\"link_lost\"} 0\n"
    "dragonbreath_fault{cause=\"panic_off\"} 0\n"
    "dragonbreath_fault{cause=\"inhibited\"} 0\n"
    "dragonbreath_fault{cause=\"emergency\"} 0\n"
    "dragonbreath_fault{cause=\"nvs_unreadable\"} 0\n"
    "# HELP dragonbreath_heater_inhibited 1 if heat is inhibited until reboot.\n"
    "# TYPE dragonbreath_heater_inhibited gauge\n"
    "dragonbreath_heater_inhibited 0\n"
    "# HELP dragonbreath_source_connected 1 if the control source link is up.\n"
    "# TYPE dragonbreath_source_connected gauge\n"
    "dragonbreath_source_connected 1\n"
    "# HELP dragonbreath_safety_trips_total Safety trips since boot, by cause. Panic-offs are in dragonbreath_panic_offs_total.\n"
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
    "# HELP dragonbreath_control_tick_duration_seconds Duration of the last control tick.\n"
    "# TYPE dragonbreath_control_tick_duration_seconds gauge\n"
    "dragonbreath_control_tick_duration_seconds 0.000850\n"
    "# HELP dragonbreath_control_tick_duration_max_seconds Longest control tick since boot.\n"
    "# TYPE dragonbreath_control_tick_duration_max_seconds gauge\n"
    "dragonbreath_control_tick_duration_max_seconds 0.041000\n"
    "# HELP dragonbreath_control_tick_interval_max_seconds Longest time between the starts of two control ticks since boot.\n"
    "# TYPE dragonbreath_control_tick_interval_max_seconds gauge\n"
    "dragonbreath_control_tick_interval_max_seconds 0.512000\n"
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
    "# HELP dragonbreath_task_instances Number of tasks with this name.\n"
    "# TYPE dragonbreath_task_instances gauge\n"
    "dragonbreath_task_instances{task=\"pb_control\"} 1\n"
    "dragonbreath_task_instances{task=\"IDLE\"} 2\n"
    "# HELP dragonbreath_task_stack_free_minimum_bytes Stack high-water mark: the least free stack since start (minimum over tasks with this name).\n"
    "# TYPE dragonbreath_task_stack_free_minimum_bytes gauge\n"
    "dragonbreath_task_stack_free_minimum_bytes{task=\"pb_control\"} 1800\n"
    "dragonbreath_task_stack_free_minimum_bytes{task=\"IDLE\"} 500\n"
    "# HELP dragonbreath_task_cpu_seconds_total CPU time of the tasks with this name. rate() gives the CPU share.\n"
    "# TYPE dragonbreath_task_cpu_seconds_total counter\n"
    "dragonbreath_task_cpu_seconds_total{task=\"pb_control\"} 1.234567\n"
    "dragonbreath_task_cpu_seconds_total{task=\"IDLE\"} 1.000005\n"
    "# HELP dragonbreath_uptime_seconds Time since boot.\n"
    "# TYPE dragonbreath_uptime_seconds gauge\n"
    "dragonbreath_uptime_seconds 3600.005\n"
    "# HELP dragonbreath_boots_total Boot count, kept in NVS. A factory reset sets it to 0. No sample if NVS failed.\n"
    "# TYPE dragonbreath_boots_total counter\n"
    "dragonbreath_boots_total 42\n"
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
    "dragonbreath_http_open_sockets 2\n";

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
    long n = format(&s);
    CHECK(n > 0);
    CHECK((size_t)n == cap.len);
    if (strcmp(cap.data, k_expected) != 0) diff_fail(cap.data, k_expected);
}

// The chunk size must not change the text: 1 byte, 64 bytes, exact, large.
static void test_chunk_sizes(void)
{
    pb_metrics_snapshot_t s;
    known_snapshot(&s);
    const size_t len = strlen(k_expected);
    const size_t sizes[] = { 1, 2, 7, 64, 1024, len, len + 1, 20000 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        long n = format_with(&s, sizes[i]);
        CHECK(n == (long)len);
        CHECK(strcmp(cap.data, k_expected) == 0);
        CHECK(cap.max_piece <= sizes[i]);
    }
    // 64-byte chunks: each piece except the last is full.
    CHECK(format_with(&s, 64) == (long)len);
    CHECK(cap.calls == (int)((len + 63) / 64));
}

static void test_sink_failure(void)
{
    pb_metrics_snapshot_t s;
    known_snapshot(&s);
    char chunk[64];
    for (int fail_after = 0; fail_after < 5; fail_after++) {
        memset(&cap, 0, sizeof cap);
        cap.fail_after = fail_after;
        CHECK(pb_metrics_format(&s, chunk, sizeof chunk, capture_sink, &cap) == -1);
        CHECK(cap.calls == fail_after);   // no sink call after the failure
    }
    CHECK(pb_metrics_format(NULL, chunk, sizeof chunk, capture_sink, &cap) == -1);
    CHECK(pb_metrics_format(&s, NULL, sizeof chunk, capture_sink, &cap) == -1);
    CHECK(pb_metrics_format(&s, chunk, 0, capture_sink, &cap) == -1);
    CHECK(pb_metrics_format(&s, chunk, sizeof chunk, NULL, &cap) == -1);
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
        CHECK(format(&s) > 0);
        char line[96];
        snprintf(line, sizeof line,
                 "\ndragonbreath_temperature_celsius{sensor=\"chamber\"} %s\n", cases[i].text);
        CHECK(strstr(cap.data, line) != NULL);
        snprintf(line, sizeof line, "\ndragonbreath_target_celsius %s\n", cases[i].text);
        CHECK(strstr(cap.data, line) != NULL);
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
    s.boot_count = 0;
    CHECK(format(&s) > 0);
    const char *o = cap.data;
    CHECK(strstr(o, "sensor=\"chamber\"} ") == NULL);
    CHECK(strstr(o, "\ndragonbreath_temperature_celsius{sensor=\"ptc\"} 73.1\n") != NULL);
    CHECK(strstr(o, "\ndragonbreath_sensor_status{sensor=\"chamber\",status=\"open\"} 1\n") != NULL);
    CHECK(strstr(o, "\ndragonbreath_sensor_status{sensor=\"chamber\",status=\"ok\"} 0\n") != NULL);
    CHECK(strstr(o, "# TYPE dragonbreath_zone_target_celsius gauge\n# HELP") != NULL);
    CHECK(strstr(o, "\ndragonbreath_wifi_rssi_dbm ") == NULL);
    CHECK(strstr(o, "\ndragonbreath_http_open_sockets ") == NULL);
    CHECK(strstr(o, "\ndragonbreath_boots_total ") == NULL);
    CHECK(strstr(o, "# TYPE dragonbreath_boots_total counter\n# HELP") != NULL);
    CHECK(strstr(o, "NaN") == NULL && strstr(o, "nan") == NULL);
}

// Without the trace facility or the run-time stats, the task families are not
// in the output.
static void test_task_families_optional(void)
{
    pb_metrics_snapshot_t s;
    known_snapshot(&s);
    s.task_cpu_valid = false;
    CHECK(format(&s) > 0);
    CHECK(strstr(cap.data, "dragonbreath_task_cpu_seconds_total") == NULL);
    CHECK(strstr(cap.data, "\ndragonbreath_task_stack_free_minimum_bytes{task=\"IDLE\"} 500\n") != NULL);
    s.task_stack_valid = false;
    CHECK(format(&s) > 0);
    CHECK(strstr(cap.data, "dragonbreath_task") == NULL);
    CHECK(strstr(cap.data, "dragonbreath_tasks") == NULL);
}

static void test_label_escaping(void)
{
    pb_metrics_snapshot_t s;
    known_snapshot(&s);
    strcpy(s.tasks[0].name, "a\"b\\c\nd");
    s.version = "1.0\"x";
    s.idf_version = NULL;
    CHECK(format(&s) > 0);
    CHECK(strstr(cap.data, "{task=\"a\\\"b\\\\c\\nd\"} 1800\n") != NULL);
    CHECK(strstr(cap.data, "{version=\"1.0\\\"x\",idf_version=\"\"} 1\n") != NULL);
}

static void test_state_sets_and_ranges(void)
{
    pb_metrics_snapshot_t s;
    known_snapshot(&s);
    s.mode = 9;                        // out of range: every mode is 0
    s.fault_code = 6;                  // panic_off
    s.reset_reason = 200;
    s.n_tasks = 60;                    // capped at PB_METRICS_MAX_TASKS
    CHECK(format(&s) > 0);
    CHECK(strstr(cap.data, "\ndragonbreath_mode{mode=\"off\"} 0\n") != NULL);
    CHECK(strstr(cap.data, "\ndragonbreath_mode{mode=\"auto\"} 0\n") != NULL);
    CHECK(strstr(cap.data, "\ndragonbreath_fault{cause=\"none\"} 0\n") != NULL);
    CHECK(strstr(cap.data, "\ndragonbreath_fault{cause=\"panic_off\"} 1\n") != NULL);
    CHECK(strstr(cap.data, "dragonbreath_reset_reason_info{reason=\"unknown\"} 1\n") != NULL);
    CHECK(strcmp(pb_metrics_reset_reason_str(1), "poweron") == 0);
    CHECK(strcmp(pb_metrics_reset_reason_str(15), "cpu_lockup") == 0);
    CHECK(strcmp(pb_metrics_reset_reason_str(PB_METRICS_RESET_REASONS), "unknown") == 0);
}

static void test_add_task(void)
{
    pb_metrics_snapshot_t s;
    memset(&s, 0, sizeof s);
    // The same tasks in two orders give the same entries.
    CHECK(pb_metrics_add_task(&s, "db_sse", 900, 10));
    CHECK(pb_metrics_add_task(&s, "db_sse", 700, 20));
    CHECK(pb_metrics_add_task(&s, "db_sse", 800, 30));
    CHECK(s.n_tasks == 1 && s.tasks[0].instances == 3);
    CHECK(s.tasks[0].stack_free_min_bytes == 700 && s.tasks[0].cpu_us == 60);
    // A long name is cut to PB_METRICS_TASK_NAME_LEN and still matches.
    CHECK(pb_metrics_add_task(&s, "abcdefghijklmnopqrstuvwxyz", 1, 1));
    CHECK(pb_metrics_add_task(&s, "abcdefghijklmnopZZZ", 2, 2));
    CHECK(s.n_tasks == 2 && strlen(s.tasks[1].name) == PB_METRICS_TASK_NAME_LEN);
    CHECK(s.tasks[1].instances == 2);
    // Full table: a new name is refused, a known name still adds.
    memset(&s, 0, sizeof s);
    for (int i = 0; i < PB_METRICS_MAX_TASKS; i++) {
        char name[8];
        snprintf(name, sizeof name, "t%d", i);
        CHECK(pb_metrics_add_task(&s, name, 100, 1));
    }
    CHECK(!pb_metrics_add_task(&s, "extra", 100, 1));
    CHECK(pb_metrics_add_task(&s, "t3", 50, 1));
    CHECK(s.n_tasks == PB_METRICS_MAX_TASKS && s.tasks[3].stack_free_min_bytes == 50);
    CHECK(!pb_metrics_add_task(NULL, "x", 0, 0) && !pb_metrics_add_task(&s, NULL, 0, 0));
}

static void test_tick_update(void)
{
    pb_metrics_tick_state_t st;
    memset(&st, 0, sizeof st);
    // First tick: no interval, never late (not scheduled).
    pb_metrics_tick_update(&st, 1000000, 1000800, 0);
    CHECK(st.ticks == 1 && st.late == 0 && st.last_us == 800 && st.max_us == 800);
    CHECK(st.interval_max_us == 0 && st.prev_start_us == 1000000);
    // On time, then exactly at the limit: not late.
    pb_metrics_tick_update(&st, 1500000, 1500300, 1500000);
    pb_metrics_tick_update(&st, 2250000, 2250100, 2000000);
    CHECK(st.late == 0 && st.last_us == 100 && st.max_us == 800);
    CHECK(st.interval_max_us == 750000);
    // 1 us over the limit: late.
    pb_metrics_tick_update(&st, 3000001, 3000100, 2750000);
    CHECK(st.late == 1);
    // A notified wake is never late, even long after the last schedule.
    pb_metrics_tick_update(&st, 9000000, 9000050, 0);
    CHECK(st.late == 1 && st.ticks == 5 && st.interval_max_us == 5999999);
    // Clamps: negative duration gives 0, a huge one gives UINT32_MAX.
    pb_metrics_tick_update(&st, 9500000, 9400000, 0);
    CHECK(st.last_us == 0);
    pb_metrics_tick_update(&st, 10000000, 10000000 + 5000000000LL, 0);
    CHECK(st.last_us == UINT32_MAX && st.max_us == UINT32_MAX);
    pb_metrics_tick_update(&st, 10000000 + 6000000000LL, 10000000 + 6000000001LL, 0);
    CHECK(st.interval_max_us == UINT32_MAX);
}

static void test_tenths(void)
{
    CHECK(pb_metrics_tenths(36.24f) == 362);
    CHECK(pb_metrics_tenths(36.25f) == 363);     // 36.25 is exact in binary: half away from 0
    CHECK(pb_metrics_tenths(-5.25f) == -53);
    CHECK(pb_metrics_tenths(-0.04f) == 0);
    CHECK(pb_metrics_tenths(-0.06f) == -1);
    CHECK(pb_metrics_tenths(0.0f) == 0);
    CHECK(pb_metrics_tenths(105.0f) == 1050);
    CHECK(pb_metrics_tenths(NAN) == PB_METRICS_TEMP_UNKNOWN);
    CHECK(pb_metrics_tenths(INFINITY) == PB_METRICS_TEMP_UNKNOWN);
    CHECK(pb_metrics_tenths(-INFINITY) == PB_METRICS_TEMP_UNKNOWN);
    CHECK(pb_metrics_tenths(100001.0f) == PB_METRICS_TEMP_UNKNOWN);
    CHECK(pb_metrics_tenths(-100001.0f) == PB_METRICS_TEMP_UNKNOWN);
}

// Worst case on the device: all task slots used, 15-character names, 31-character
// version strings (the app descriptor fields are char[32]) and maximum values.
// The response streams, so this is for information (docs) only.
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
    s->n_tasks = 0;
    for (int i = 0; i < PB_METRICS_MAX_TASKS; i++) {
        char name[PB_METRICS_TASK_NAME_LEN + 1];
        snprintf(name, sizeof name, "abcdefghijklm%02d", i);
        CHECK(pb_metrics_add_task(s, name, UINT32_MAX, UINT64_MAX / 2));
    }
    s->boot_count = UINT32_MAX;
    s->version = "v1.2.3-45-gabcdef0-dirty-abcdef";
    s->idf_version = "v5.3.6-dirty-abcdefghijklmnopqr";
    s->rssi_dbm = -128;
    s->http_sockets = INT16_MAX;
}

static void test_worst_case_size(void)
{
    pb_metrics_snapshot_t s;
    worst_case_snapshot(&s);
    long n = format(&s);
    CHECK(n > 0 && (size_t)n == cap.len);
    printf("worst-case /metrics size: %ld bytes\n", n);
}

// A realistic device snapshot for the parse check.
static void dump(void)
{
    pb_metrics_snapshot_t s;
    known_snapshot(&s);
    s.chamber_dc = -52;
    s.ptc_dc = PB_METRICS_TEMP_UNKNOWN;
    s.ptc_status = 2;
    static const char *const names[] = {
        "IDLE", "Tmr Svc", "esp_timer", "sys_evt", "tiT", "wifi", "httpd",
        "pb_control", "pb_buttons", "pb_leds", "mdns", "dc_prusa", "db_sse",
        "db_sse", "evlog \"q\\",
    };
    s.n_tasks = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        CHECK(pb_metrics_add_task(&s, names[i], 1000 + (uint32_t)i, 1000000ull * i + 7));
    s.task_count = (uint16_t)(sizeof names / sizeof names[0]);
    s.version = "abc\"1";
    CHECK(format(&s) > 0);
    fputs(cap.data, stdout);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        dump();
        return 0;
    }
    test_known_snapshot();
    test_chunk_sizes();
    test_sink_failure();
    test_negative_temperatures();
    test_unknown_values();
    test_task_families_optional();
    test_label_escaping();
    test_state_sets_and_ranges();
    test_add_task();
    test_tick_update();
    test_tenths();
    test_worst_case_size();
    printf("pb_metrics_fmt host test: PASS\n");
    return 0;
}
