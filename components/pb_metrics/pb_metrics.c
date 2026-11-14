// SPDX-License-Identifier: MIT
// pb_metrics — control-tick timing, boot count and reset reason.
#include "pb_metrics.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

static const char *TAG = "pb_metrics";

#define NVS_NS        "app_nvs"
#define KEY_BOOT_CNT  "boot_cnt"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static pb_metrics_tick_stats_t s_tick;   // guarded by s_mux
static int64_t s_prev_start_us;          // guarded by s_mux; 0 before the first tick

static uint32_t s_boot_count;            // written once at boot
static unsigned s_reset_reason;          // written once at boot

static uint32_t clamp_u32(int64_t v)
{
    if (v < 0) return 0;
    return v > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

void pb_metrics_tick_record(int64_t start_us, int64_t end_us, int64_t scheduled_us)
{
    uint32_t duration = clamp_u32(end_us - start_us);
    bool late = scheduled_us != 0 && start_us - scheduled_us > PB_METRICS_TICK_LATE_US;
    taskENTER_CRITICAL(&s_mux);
    s_tick.ticks++;
    if (late) s_tick.late++;
    s_tick.last_us = duration;
    if (duration > s_tick.max_us) s_tick.max_us = duration;
    if (s_prev_start_us != 0) {
        uint32_t interval = clamp_u32(start_us - s_prev_start_us);
        if (interval > s_tick.interval_max_us) s_tick.interval_max_us = interval;
    }
    s_prev_start_us = start_us;
    taskEXIT_CRITICAL(&s_mux);
}

void pb_metrics_get_tick_stats(pb_metrics_tick_stats_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_mux);
    *out = s_tick;
    taskEXIT_CRITICAL(&s_mux);
}

void pb_metrics_boot_init(void)
{
    s_reset_reason = (unsigned)esp_reset_reason();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "boot count: nvs_open failed");
        return;
    }
    uint32_t n = 0;
    (void)nvs_get_u32(h, KEY_BOOT_CNT, &n);   // not found: first boot, n stays 0
    n++;
    if (nvs_set_u32(h, KEY_BOOT_CNT, n) == ESP_OK && nvs_commit(h) == ESP_OK)
        s_boot_count = n;
    else
        ESP_LOGW(TAG, "boot count: NVS write failed");
    nvs_close(h);
    ESP_LOGI(TAG, "boot %lu, reset reason %u", (unsigned long)n, s_reset_reason);
}

uint32_t pb_metrics_boot_count(void) { return s_boot_count; }
unsigned pb_metrics_reset_reason(void) { return s_reset_reason; }
