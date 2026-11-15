// SPDX-License-Identifier: MIT
// GET /metrics: collect a pb_metrics_snapshot_t and stream it in Prometheus
// text format. The pure writer (pb_metrics_fmt) does the formatting.
//
// Cost on each scrape: the snapshot (about 1.1 KB) and the FreeRTOS task list
// on the heap, both freed before the handler returns, and a 1 KB chunk buffer
// on the httpd stack. The response uses chunked transfer encoding, so its size
// has no fixed limit. The policy lock is held only by pb_policy_get_snapshot().
#include "pb_httpd_metrics.h"
#include "pb_metrics.h"
#include "pb_metrics_fmt.h"
#include "pb_heater.h"
#include "pb_netlog.h"
#include "pb_ntc.h"
#include "pb_policy.h"

#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pb_metrics_http";

#define METRICS_CHUNK_SIZE 1024   // on the httpd task stack (8 KB)

// The writer has its own name tables. They must follow the firmware enums.
_Static_assert(PB_FAULT__COUNT == PB_METRICS_FAULT_CAUSES, "fault cause table size");
_Static_assert(PB_FAULT_NONE == PB_METRICS_CAUSE_NONE &&
               PB_FAULT_PTC_OVERTEMP == PB_METRICS_CAUSE_PTC_OVERTEMP &&
               PB_FAULT_CHAMBER_OVERTEMP == PB_METRICS_CAUSE_CHAMBER_OVERTEMP &&
               PB_FAULT_CHAMBER_SENSOR == PB_METRICS_CAUSE_CHAMBER_SENSOR &&
               PB_FAULT_PTC_SENSOR == PB_METRICS_CAUSE_PTC_SENSOR &&
               PB_FAULT_LINK_LOST == PB_METRICS_CAUSE_LINK_LOST &&
               PB_FAULT_PANIC_OFF == PB_METRICS_CAUSE_PANIC_OFF &&
               PB_FAULT_INHIBITED == PB_METRICS_CAUSE_INHIBITED &&
               PB_FAULT_EMERGENCY == PB_METRICS_CAUSE_EMERGENCY &&
               PB_FAULT_NVS_UNREADABLE == PB_METRICS_CAUSE_NVS_UNREADABLE &&
               PB_FAULT_HEATER_NO_RISE == PB_METRICS_CAUSE_HEATER_NO_RISE &&
               PB_FAULT_SENSOR_ROLES == PB_METRICS_CAUSE_SENSOR_ROLES,
               "fault cause table order");
_Static_assert(PB_MODE_OFF == 0 && PB_MODE_POWER_ON == 1 && PB_MODE_AUTO == 2 &&
               PB_MODE_DRYING == 3, "mode table order");
_Static_assert(PB_NTC_OK == 0 && PB_NTC_SHORT == 1 && PB_NTC_OPEN == 2 &&
               PB_NTC_UNINIT == 3, "sensor status table order");
_Static_assert(configMAX_TASK_NAME_LEN - 1 <= PB_METRICS_TASK_NAME_LEN, "task name length");

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
// A copy of one task entry. The name is copied while the scheduler is
// suspended, so it stays valid if the task is deleted later.
typedef struct {
    char name[configMAX_TASK_NAME_LEN];
    uint32_t stack_free;
    uint64_t cpu_us;
} raw_task_t;

static void collect_tasks(pb_metrics_snapshot_t *m)
{
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;   // room for tasks created meanwhile
    TaskStatus_t *ts = malloc(cap * sizeof *ts);
    raw_task_t *raw = malloc(cap * sizeof *raw);
    if (!ts || !raw) {
        free(ts);
        free(raw);
        return;
    }
    // Short: copy only. The idle task cannot free a TCB (and its name) meanwhile.
    vTaskSuspendAll();
    UBaseType_t got = uxTaskGetSystemState(ts, cap, NULL);
    for (UBaseType_t i = 0; i < got; i++) {
        strncpy(raw[i].name, ts[i].pcTaskName, sizeof raw[i].name - 1);
        raw[i].name[sizeof raw[i].name - 1] = '\0';
        raw[i].stack_free = ts[i].usStackHighWaterMark;   // bytes on ESP-IDF
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        raw[i].cpu_us = ts[i].ulRunTimeCounter;
#else
        raw[i].cpu_us = 0;
#endif
    }
    xTaskResumeAll();
    free(ts);

    // One series for each name: the order of uxTaskGetSystemState() can change,
    // so a per-instance suffix would move counters between series.
    m->task_count = (uint16_t)got;
    for (UBaseType_t i = 0; i < got; i++)
        (void)pb_metrics_add_task(m, raw[i].name, raw[i].stack_free, raw[i].cpu_us);
    free(raw);
    m->task_stack_valid = got > 0;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    m->task_cpu_valid = got > 0;
#endif
}
#endif

static void collect(pb_metrics_snapshot_t *m, httpd_req_t *req,
                    uint32_t heap_free, uint32_t heap_min, uint32_t heap_largest)
{
    memset(m, 0, sizeof *m);

    pb_policy_snapshot_t s;
    pb_policy_get_snapshot(&s);   // takes and releases the policy lock once
    m->chamber_dc = pb_metrics_tenths(s.chamber_c);
    m->ptc_dc = pb_metrics_tenths(s.ptc_c);
    m->chamber_status = (uint8_t)s.chamber_status;
    m->ptc_status = (uint8_t)s.ptc_status;
    m->heater_demand = s.heater_demand;
    m->relay_on = s.heater_output;
    m->fan_percent = s.effective_fan_percent;
    m->mode = (uint8_t)s.mode;
    m->target_dc = pb_metrics_tenths(s.effective_target_c);
    m->zone_target_dc = s.source_target_c > 0.0f
        ? pb_metrics_tenths(s.source_target_c) : PB_METRICS_TEMP_UNKNOWN;
    m->fault_latched = s.fault_latched;
    m->inhibited = s.inhibited;
    m->fault_code = (uint8_t)pb_heater_fault_code();
    m->source_connected = s.moonraker_connected;   // set for every control source

    pb_heater_counters_t hc;
    pb_heater_get_counters(&hc);
    memcpy(m->trips, hc.trips, sizeof m->trips);
    m->panic_offs = hc.panic_offs;
    m->relay_on_ms = hc.relay_on_us / 1000;

    pb_metrics_tick_state_t tk;
    pb_metrics_get_tick_stats(&tk);
    m->ticks = tk.ticks;
    m->ticks_late = tk.late;
    m->tick_last_us = tk.last_us;
    m->tick_max_us = tk.max_us;
    m->tick_interval_max_us = tk.interval_max_us;

    m->heap_free = heap_free;
    m->heap_min_free = heap_min;
    m->heap_largest_block = heap_largest;

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    collect_tasks(m);
#endif

    m->uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);
    m->boot_count = pb_metrics_boot_count();
    m->reset_reason = (uint8_t)pb_metrics_reset_reason();
    const esp_app_desc_t *app = esp_app_get_description();
    m->version = app->version;
    m->idf_version = app->idf_ver;

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        m->rssi_valid = true;
        m->rssi_dbm = ap.rssi;
    }
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    size_t nfds = sizeof fds / sizeof fds[0];
    m->http_sockets = httpd_get_client_list(req->handle, &nfds, fds) == ESP_OK
        ? (int16_t)nfds : -1;

    pb_netlog_stats_t nl;
    pb_netlog_get_stats(&nl);
    m->syslog_enabled = nl.syslog_enabled;
    m->syslog_sent = nl.syslog_sent;
    m->syslog_dropped = nl.syslog_dropped;
    m->syslog_send_errors = nl.syslog_send_errors;
    m->syslog_queued = nl.syslog_queued;
    m->time_synced = nl.time_synced;
}

static bool send_chunk(void *ctx, const char *data, size_t len)
{
    return httpd_resp_send_chunk((httpd_req_t *)ctx, data, (ssize_t)len) == ESP_OK;
}

esp_err_t pb_httpd_metrics_get(httpd_req_t *req)
{
    // Read the heap figures before this handler allocates its own buffers.
    uint32_t heap_free = esp_get_free_heap_size();
    uint32_t heap_min = esp_get_minimum_free_heap_size();
    uint32_t heap_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    pb_metrics_snapshot_t *snap = malloc(sizeof *snap);
    if (!snap) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "metrics: out of memory");
        return ESP_FAIL;
    }
    collect(snap, req, heap_free, heap_min, heap_largest);

    char chunk[METRICS_CHUNK_SIZE];
    httpd_resp_set_type(req, "text/plain; version=0.0.4; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    long n = pb_metrics_format(snap, chunk, sizeof chunk, send_chunk, req);
    free(snap);
    if (n < 0) {
        // A chunk send failed (client gone). The headers can be out already, so
        // an error status is not possible. ESP_FAIL makes httpd close the socket.
        ESP_LOGD(TAG, "metrics send failed");
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}
