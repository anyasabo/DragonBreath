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

// pb_metrics_reset_reason_str() has one name for each esp_reset_reason_t value.
_Static_assert(ESP_RST_UNKNOWN == 0 && ESP_RST_POWERON == 1 && ESP_RST_EXT == 2 &&
               ESP_RST_SW == 3 && ESP_RST_PANIC == 4 && ESP_RST_INT_WDT == 5 &&
               ESP_RST_TASK_WDT == 6 && ESP_RST_WDT == 7 && ESP_RST_DEEPSLEEP == 8 &&
               ESP_RST_BROWNOUT == 9 && ESP_RST_SDIO == 10 && ESP_RST_USB == 11 &&
               ESP_RST_JTAG == 12 && ESP_RST_EFUSE == 13 && ESP_RST_PWR_GLITCH == 14,
               "reset reason name table order");
_Static_assert(ESP_RST_CPU_LOCKUP + 1 == PB_METRICS_RESET_REASONS,
               "reset reason name table size");

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static pb_metrics_tick_state_t s_tick;   // guarded by s_mux

static uint32_t s_boot_count;            // written once at boot; 0 = unknown
static unsigned s_reset_reason;          // written once at boot

void pb_metrics_tick_record(int64_t start_us, int64_t end_us, int64_t scheduled_us)
{
    taskENTER_CRITICAL(&s_mux);
    pb_metrics_tick_update(&s_tick, start_us, end_us, scheduled_us);
    taskEXIT_CRITICAL(&s_mux);
}

void pb_metrics_get_tick_stats(pb_metrics_tick_state_t *out)
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
        ESP_LOGW(TAG, "boot count: nvs_open failed, reset reason %u", s_reset_reason);
        return;
    }
    uint32_t n = 0;
    (void)nvs_get_u32(h, KEY_BOOT_CNT, &n);   // not found: first boot, n stays 0
    n++;
    if (nvs_set_u32(h, KEY_BOOT_CNT, n) == ESP_OK && nvs_commit(h) == ESP_OK) {
        s_boot_count = n;
        ESP_LOGI(TAG, "boot %lu, reset reason %u", (unsigned long)n, s_reset_reason);
    } else {
        ESP_LOGW(TAG, "boot count: NVS write failed, reset reason %u", s_reset_reason);
    }
    nvs_close(h);
}

uint32_t pb_metrics_boot_count(void) { return s_boot_count; }
unsigned pb_metrics_reset_reason(void) { return s_reset_reason; }
