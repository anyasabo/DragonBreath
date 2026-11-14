// SPDX-License-Identifier: MIT
// GET /metrics: collect a pb_metrics_snapshot_t and send it in Prometheus text
// format. The pure writer (pb_metrics_fmt) does the formatting.
//
// Cost on each scrape: one heap block (snapshot + PB_METRICS_BUF_SIZE text
// buffer) and one small block for the FreeRTOS task list. Both are freed before
// the handler returns. The policy lock is held only by pb_policy_get_snapshot().
#include "pb_httpd_metrics.h"
#include "pb_metrics.h"
#include "pb_metrics_fmt.h"
#include "pb_heater.h"
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
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pb_metrics_http";

// The writer has its own name tables. They must follow the firmware enums.
_Static_assert(PB_FAULT__COUNT == PB_METRICS_FAULT_CAUSES, "fault cause table size");
_Static_assert(PB_FAULT_PANIC_OFF == PB_METRICS_CAUSE_PANIC_OFF, "panic-off cause index");
_Static_assert(PB_FAULT_NVS_UNREADABLE == PB_METRICS_CAUSE_NVS_UNREADABLE, "nvs cause index");
_Static_assert(PB_MODE_OFF == 0 && PB_MODE_POWER_ON == 1 && PB_MODE_AUTO == 2 &&
               PB_MODE_DRYING == 3, "mode table order");
_Static_assert(PB_NTC_OK == 0 && PB_NTC_SHORT == 1 && PB_NTC_OPEN == 2 &&
               PB_NTC_UNINIT == 3, "sensor status table order");

// Only the single httpd worker task runs this handler.
static uint32_t s_last_build_us;

typedef struct {
    pb_metrics_snapshot_t snap;
    char text[PB_METRICS_BUF_SIZE];
} metrics_work_t;

// Degrees C to tenths with rounding. Not finite: PB_METRICS_TEMP_UNKNOWN.
static int32_t to_tenths(float c)
{
    if (!isfinite(c) || c > 100000.0f || c < -100000.0f) return PB_METRICS_TEMP_UNKNOWN;
    float t = c * 10.0f;
    return (int32_t)(t >= 0.0f ? t + 0.5f : t - 0.5f);
}

// Copy the task list. A name that is already in the list gets a "#N" suffix,
// so each series has a unique label set (for example two "db_sse" tasks).
static void collect_tasks(pb_metrics_snapshot_t *m)
{
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;   // room for tasks created meanwhile
    TaskStatus_t *ts = malloc(cap * sizeof *ts);
    if (!ts) return;
    // Hold the scheduler while names are copied: the idle task cannot free the
    // TCB of a deleted task (and its name) during this short loop.
    vTaskSuspendAll();
    UBaseType_t got = uxTaskGetSystemState(ts, cap, NULL);
    m->task_count = (uint16_t)uxTaskGetNumberOfTasks();
    for (UBaseType_t i = 0; i < got && m->n_tasks < PB_METRICS_MAX_TASKS; i++) {
        pb_metrics_task_t *t = &m->tasks[m->n_tasks];
        unsigned dup = 0;
        for (UBaseType_t j = 0; j < i; j++)
            if (strcmp(ts[j].pcTaskName, ts[i].pcTaskName) == 0) dup++;
        strncpy(t->name, ts[i].pcTaskName, PB_METRICS_TASK_NAME_LEN);
        t->name[PB_METRICS_TASK_NAME_LEN] = '\0';
        if (dup) {
            size_t len = strlen(t->name);
            if (len > PB_METRICS_TASK_NAME_LEN - 3) len = PB_METRICS_TASK_NAME_LEN - 3;
            t->name[len++] = '#';
            if (dup + 1 >= 10) t->name[len++] = (char)('0' + (dup + 1) / 10 % 10);
            t->name[len++] = (char)('0' + (dup + 1) % 10);
            t->name[len] = '\0';
        }
        t->stack_free_min_bytes = ts[i].usStackHighWaterMark;   // bytes on ESP-IDF
        t->cpu_us = ts[i].ulRunTimeCounter;
        m->n_tasks++;
    }
    xTaskResumeAll();
    free(ts);
}

static void collect(pb_metrics_snapshot_t *m, httpd_req_t *req,
                    uint32_t heap_free, uint32_t heap_min, uint32_t heap_largest)
{
    memset(m, 0, sizeof *m);

    pb_policy_snapshot_t s;
    pb_policy_get_snapshot(&s);   // takes and releases the policy lock once
    m->chamber_dc = to_tenths(s.chamber_c);
    m->ptc_dc = to_tenths(s.ptc_c);
    m->chamber_status = (uint8_t)s.chamber_status;
    m->ptc_status = (uint8_t)s.ptc_status;
    m->heater_demand = s.heater_demand;
    m->relay_on = s.heater_output;
    m->fan_percent = s.effective_fan_percent;
    m->mode = (uint8_t)s.mode;
    m->target_dc = to_tenths(s.effective_target_c);
    m->zone_target_dc = s.source_target_c > 0.0f
        ? to_tenths(s.source_target_c) : PB_METRICS_TEMP_UNKNOWN;
    m->fault_latched = s.fault_latched;
    m->inhibited = s.inhibited;
    m->fault_code = (uint8_t)pb_heater_fault_code();
    m->source_connected = s.moonraker_connected;   // set for every control source

    pb_heater_counters_t hc;
    pb_heater_get_counters(&hc);
    memcpy(m->trips, hc.trips, sizeof m->trips);
    m->panic_offs = hc.panic_offs;
    m->relay_on_ms = hc.relay_on_us / 1000;

    pb_metrics_tick_stats_t tk;
    pb_metrics_get_tick_stats(&tk);
    m->ticks = tk.ticks;
    m->ticks_late = tk.late;
    m->tick_last_us = tk.last_us;
    m->tick_max_us = tk.max_us;
    m->tick_interval_max_us = tk.interval_max_us;

    m->heap_free = heap_free;
    m->heap_min_free = heap_min;
    m->heap_largest_block = heap_largest;

    collect_tasks(m);

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
    m->last_build_us = s_last_build_us;
}

esp_err_t pb_httpd_metrics_get(httpd_req_t *req)
{
    const int64_t t0 = esp_timer_get_time();
    // Read the heap figures before this handler allocates its own buffers.
    uint32_t heap_free = esp_get_free_heap_size();
    uint32_t heap_min = esp_get_minimum_free_heap_size();
    uint32_t heap_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    metrics_work_t *w = malloc(sizeof *w);
    if (!w) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "metrics: out of memory");
        return ESP_FAIL;
    }
    collect(&w->snap, req, heap_free, heap_min, heap_largest);
    int n = pb_metrics_format(&w->snap, w->text, sizeof w->text);
    int64_t dt = esp_timer_get_time() - t0;
    s_last_build_us = dt > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)dt;
    if (n < 0) {
        free(w);
        ESP_LOGW(TAG, "metrics text does not fit in %d bytes", PB_METRICS_BUF_SIZE);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "metrics: buffer too small");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain; version=0.0.4; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, w->text, n);
    free(w);
    return r;
}
