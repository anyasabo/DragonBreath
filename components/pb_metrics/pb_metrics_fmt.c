// SPDX-License-Identifier: MIT
// pb_metrics_fmt — Prometheus text format 0.0.4 writer. Pure C: integer math
// only, no printf, no float, no malloc. See pb_metrics_fmt.h.
#include "pb_metrics_fmt.h"

#include <string.h>

// Keep these tables in the order of the firmware enums named in the comments.
static const char *const k_modes[] = {      // pb_mode_t
    "off", "power_on", "auto", "drying",
};
static const char *const k_ntc_status[] = { // pb_ntc_status_t
    "ok", "short", "open", "uninit",
};
static const char *const k_causes[PB_METRICS_FAULT_CAUSES] = {  // pb_fault_reason_t
    "none", "ptc_overtemp", "chamber_overtemp", "chamber_sensor", "ptc_sensor",
    "link_lost", "panic_off", "inhibited", "emergency", "nvs_unreadable",
};
static const char *const k_reset_reasons[] = {  // esp_reset_reason_t
    "unknown", "poweron", "ext", "sw", "panic", "int_wdt", "task_wdt", "wdt",
    "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse", "pwr_glitch",
    "cpu_lockup",
};

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

const char *pb_metrics_reset_reason_str(unsigned reason)
{
    return reason < COUNT_OF(k_reset_reasons) ? k_reset_reasons[reason] : "unknown";
}

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
    bool labels_open;
} writer_t;

static void put(writer_t *w, const char *s, size_t n)
{
    if (w->overflow) return;
    // Keep one byte for the NUL terminator.
    if (n >= w->cap - w->len) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void put_str(writer_t *w, const char *s)
{
    put(w, s, strlen(s));
}

static void put_u64(writer_t *w, uint64_t v)
{
    char tmp[20];
    size_t i = sizeof tmp;
    do {
        tmp[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v);
    put(w, tmp + i, sizeof tmp - i);
}

static void put_i64(writer_t *w, int64_t v)
{
    if (v < 0) {
        put(w, "-", 1);
        put_u64(w, (uint64_t)0 - (uint64_t)v);
    } else {
        put_u64(w, (uint64_t)v);
    }
}

// Write v / 10^decimals with exactly `decimals` digits after the point.
static void put_fixed(writer_t *w, uint64_t v, unsigned decimals)
{
    uint64_t scale = 1;
    for (unsigned i = 0; i < decimals; i++) scale *= 10u;
    put_u64(w, v / scale);
    put(w, ".", 1);
    char frac[8];
    uint64_t f = v % scale;
    for (unsigned i = decimals; i > 0; i--) {
        frac[i - 1] = (char)('0' + (f % 10u));
        f /= 10u;
    }
    put(w, frac, decimals);
}

// Tenths of a degree as "%d.%d": -5 -> "-0.5", 362 -> "36.2".
static void put_tenths(writer_t *w, int32_t v)
{
    uint64_t mag = v < 0 ? (uint64_t)0 - (uint64_t)(int64_t)v : (uint64_t)v;
    if (v < 0) put(w, "-", 1);
    put_fixed(w, mag, 1);
}

// Label value escaping of the text format: backslash, double quote, newline.
static void put_escaped(writer_t *w, const char *s)
{
    if (!s) return;
    const char *run = s;
    for (; *s; s++) {
        const char *esc = NULL;
        if (*s == '\\') esc = "\\\\";
        else if (*s == '"') esc = "\\\"";
        else if (*s == '\n') esc = "\\n";
        if (!esc) continue;
        put(w, run, (size_t)(s - run));
        put(w, esc, 2);
        run = s + 1;
    }
    put(w, run, (size_t)(s - run));
}

static void family(writer_t *w, const char *name, const char *type, const char *help)
{
    put_str(w, "# HELP ");
    put_str(w, name);
    put(w, " ", 1);
    put_str(w, help);
    put_str(w, "\n# TYPE ");
    put_str(w, name);
    put(w, " ", 1);
    put_str(w, type);
    put(w, "\n", 1);
}

static void sample(writer_t *w, const char *name)
{
    put_str(w, name);
    w->labels_open = false;
}

static void label(writer_t *w, const char *key, const char *value)
{
    put(w, w->labels_open ? "," : "{", 1);
    w->labels_open = true;
    put_str(w, key);
    put(w, "=\"", 2);
    put_escaped(w, value);
    put(w, "\"", 1);
}

static void value_begin(writer_t *w)
{
    if (w->labels_open) put(w, "}", 1);
    w->labels_open = false;
    put(w, " ", 1);
}

static void end_u64(writer_t *w, uint64_t v)
{
    value_begin(w);
    put_u64(w, v);
    put(w, "\n", 1);
}

static void end_i64(writer_t *w, int64_t v)
{
    value_begin(w);
    put_i64(w, v);
    put(w, "\n", 1);
}

static void end_tenths(writer_t *w, int32_t v)
{
    value_begin(w);
    put_tenths(w, v);
    put(w, "\n", 1);
}

static void end_fixed(writer_t *w, uint64_t v, unsigned decimals)
{
    value_begin(w);
    put_fixed(w, v, decimals);
    put(w, "\n", 1);
}

// One family with one unlabeled integer sample.
static void gauge_u64(writer_t *w, const char *name, const char *help, uint64_t v)
{
    family(w, name, "gauge", help);
    sample(w, name);
    end_u64(w, v);
}

static void counter_u64(writer_t *w, const char *name, const char *help, uint64_t v)
{
    family(w, name, "counter", help);
    sample(w, name);
    end_u64(w, v);
}

static void sensor_temp(writer_t *w, const char *sensor, int32_t dc)
{
    if (dc == PB_METRICS_TEMP_UNKNOWN) return;   // no sample for an invalid reading
    sample(w, "dragonbreath_temperature_celsius");
    label(w, "sensor", sensor);
    end_tenths(w, dc);
}

static void sensor_status(writer_t *w, const char *sensor, uint8_t status)
{
    for (unsigned i = 0; i < COUNT_OF(k_ntc_status); i++) {
        sample(w, "dragonbreath_sensor_status");
        label(w, "sensor", sensor);
        label(w, "status", k_ntc_status[i]);
        end_u64(w, status == i);
    }
}

static void tenths_gauge(writer_t *w, const char *name, const char *help, int32_t dc)
{
    family(w, name, "gauge", help);
    if (dc == PB_METRICS_TEMP_UNKNOWN) return;
    sample(w, name);
    end_tenths(w, dc);
}

int pb_metrics_format(const pb_metrics_snapshot_t *s, char *buf, size_t cap)
{
    if (!s || !buf || cap == 0) return -1;
    writer_t w = { .buf = buf, .cap = cap };

    family(&w, "dragonbreath_temperature_celsius", "gauge",
           "Smoothed sensor temperature. No sample if the sensor status is not ok.");
    sensor_temp(&w, "chamber", s->chamber_dc);
    sensor_temp(&w, "ptc", s->ptc_dc);

    family(&w, "dragonbreath_sensor_status", "gauge",
           "Thermistor status. 1 for the current status, 0 for the others.");
    sensor_status(&w, "chamber", s->chamber_status);
    sensor_status(&w, "ptc", s->ptc_status);

    gauge_u64(&w, "dragonbreath_heater_demand", "1 if a target is armed and no fault is latched.",
              s->heater_demand);
    gauge_u64(&w, "dragonbreath_heater_relay_on", "1 if the SSR output is on.", s->relay_on);
    gauge_u64(&w, "dragonbreath_fan_percent", "Effective blower level in percent.", s->fan_percent);

    family(&w, "dragonbreath_mode", "gauge", "Control mode. 1 for the current mode, 0 for the others.");
    for (unsigned i = 0; i < COUNT_OF(k_modes); i++) {
        sample(&w, "dragonbreath_mode");
        label(&w, "mode", k_modes[i]);
        end_u64(&w, s->mode == i);
    }

    tenths_gauge(&w, "dragonbreath_target_celsius", "Effective heater target. 0 when off.",
                 s->target_dc);
    tenths_gauge(&w, "dragonbreath_zone_target_celsius",
                 "Filament-zone target from the control source. No sample if no zone is known.",
                 s->zone_target_dc);

    gauge_u64(&w, "dragonbreath_fault_latched", "1 if a safety fault or an inhibit is latched.",
              s->fault_latched);
    gauge_u64(&w, "dragonbreath_fault_code", "Cause code of the latched fault (pb_fault_reason_t, 0 = none).",
              s->fault_code);
    gauge_u64(&w, "dragonbreath_heater_inhibited", "1 if heat is inhibited until reboot.",
              s->inhibited);
    gauge_u64(&w, "dragonbreath_source_connected", "1 if the control source link is up.",
              s->source_connected);

    family(&w, "dragonbreath_safety_trips_total", "counter",
           "New safety latches since boot, by cause. Panic-offs are in dragonbreath_panic_offs_total.");
    for (unsigned i = 1; i < PB_METRICS_FAULT_CAUSES; i++) {
        // Panic-off has its own counter. nvs_unreadable occurs only as a boot restore.
        if (i == PB_METRICS_CAUSE_PANIC_OFF || i == PB_METRICS_CAUSE_NVS_UNREADABLE) continue;
        sample(&w, "dragonbreath_safety_trips_total");
        label(&w, "cause", k_causes[i]);
        end_u64(&w, s->trips[i]);
    }
    counter_u64(&w, "dragonbreath_panic_offs_total", "Front-panel panic-off requests since boot.",
                s->panic_offs);
    family(&w, "dragonbreath_relay_on_seconds_total", "counter", "Time with the SSR output on since boot.");
    sample(&w, "dragonbreath_relay_on_seconds_total");
    end_fixed(&w, s->relay_on_ms, 3);

    counter_u64(&w, "dragonbreath_control_ticks_total", "Control loop iterations since boot.", s->ticks);
    counter_u64(&w, "dragonbreath_control_ticks_late_total",
                "Periodic control ticks that started more than 250 ms after the scheduled time.",
                s->ticks_late);
    gauge_u64(&w, "dragonbreath_control_tick_duration_microseconds",
              "Duration of the last control tick.", s->tick_last_us);
    gauge_u64(&w, "dragonbreath_control_tick_duration_max_microseconds",
              "Longest control tick since boot.", s->tick_max_us);
    gauge_u64(&w, "dragonbreath_control_tick_interval_max_microseconds",
              "Longest time between the starts of two control ticks since boot.",
              s->tick_interval_max_us);

    gauge_u64(&w, "dragonbreath_heap_free_bytes", "Free heap.", s->heap_free);
    gauge_u64(&w, "dragonbreath_heap_minimum_free_bytes", "Lowest free heap since boot.",
              s->heap_min_free);
    gauge_u64(&w, "dragonbreath_heap_largest_free_block_bytes", "Largest free heap block.",
              s->heap_largest_block);

    gauge_u64(&w, "dragonbreath_tasks", "Number of FreeRTOS tasks.", s->task_count);
    unsigned n = s->n_tasks < PB_METRICS_MAX_TASKS ? s->n_tasks : PB_METRICS_MAX_TASKS;
    family(&w, "dragonbreath_task_stack_free_minimum_bytes", "gauge",
           "Stack high-water mark: the least free stack of the task since it started.");
    for (unsigned i = 0; i < n; i++) {
        sample(&w, "dragonbreath_task_stack_free_minimum_bytes");
        label(&w, "task", s->tasks[i].name);
        end_u64(&w, s->tasks[i].stack_free_min_bytes);
    }
    family(&w, "dragonbreath_task_cpu_seconds_total", "counter",
           "CPU time of the task since it started. rate() gives the CPU share.");
    for (unsigned i = 0; i < n; i++) {
        sample(&w, "dragonbreath_task_cpu_seconds_total");
        label(&w, "task", s->tasks[i].name);
        end_fixed(&w, s->tasks[i].cpu_us, 6);
    }

    family(&w, "dragonbreath_uptime_seconds", "gauge", "Time since boot.");
    sample(&w, "dragonbreath_uptime_seconds");
    end_fixed(&w, s->uptime_ms, 3);
    counter_u64(&w, "dragonbreath_boots_total", "Boot count, kept in NVS. A factory reset sets it to 0.",
                s->boot_count);
    gauge_u64(&w, "dragonbreath_reset_reason", "esp_reset_reason() of this boot (numeric).",
              s->reset_reason);
    family(&w, "dragonbreath_reset_reason_info", "gauge", "esp_reset_reason() of this boot as a label.");
    sample(&w, "dragonbreath_reset_reason_info");
    label(&w, "reason", pb_metrics_reset_reason_str(s->reset_reason));
    end_u64(&w, 1);
    family(&w, "dragonbreath_build_info", "gauge", "Firmware version.");
    sample(&w, "dragonbreath_build_info");
    label(&w, "version", s->version ? s->version : "");
    label(&w, "idf_version", s->idf_version ? s->idf_version : "");
    end_u64(&w, 1);

    family(&w, "dragonbreath_wifi_rssi_dbm", "gauge", "Wi-Fi RSSI of the station link. No sample if not connected.");
    if (s->rssi_valid) {
        sample(&w, "dragonbreath_wifi_rssi_dbm");
        end_i64(&w, s->rssi_dbm);
    }
    family(&w, "dragonbreath_http_open_sockets", "gauge", "Open client sockets of the HTTP server.");
    if (s->http_sockets >= 0) {
        sample(&w, "dragonbreath_http_open_sockets");
        end_i64(&w, s->http_sockets);
    }
    gauge_u64(&w, "dragonbreath_metrics_build_duration_microseconds",
              "Time to collect and format the previous /metrics response.", s->last_build_us);

    if (w.overflow) return -1;
    buf[w.len] = '\0';
    return (int)w.len;
}
