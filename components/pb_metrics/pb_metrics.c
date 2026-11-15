// SPDX-License-Identifier: MIT
// pb_metrics — control-tick timing, boot count, reset reason, panic
// breadcrumb, failed allocations, Wi-Fi disconnects and airflow signs.
#include "pb_metrics.h"

#include <stdbool.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "pb_panic.h"

static const char *TAG = "pb_metrics";

#define NVS_NS         "app_nvs"
#define KEY_BOOT_CNT   "boot_cnt"
#define KEY_PANIC_CNT  "panic_cnt"

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
static pb_airflow_t s_airflow;           // guarded by s_mux; written by the control task
static bool s_airflow_init;              // control task only

static uint32_t s_boot_count;            // written once at boot; 0 = unknown
static unsigned s_reset_reason;          // written once at boot
static uint32_t s_panic_count;           // written once at boot
static bool s_panic_count_valid;         // written once at boot
static pb_panic_record_t s_last_panic;   // written once at boot
static bool s_last_panic_valid;          // written once at boot

// Failed allocations. The callback can run in any task and in an ISR, so it
// uses only atomic operations (no lock, no log).
static uint32_t s_alloc_failures;
static uint32_t s_alloc_failure_last;

// Wi-Fi station disconnects (event task writes, httpd reads).
static uint32_t s_wifi_disconnects;
static uint32_t s_wifi_last_reason;

void pb_metrics_tick_record(int64_t start_us, int64_t end_us, int64_t scheduled_us,
                            const uint32_t part_us[PB_METRICS_TICK_PARTS])
{
    taskENTER_CRITICAL(&s_mux);
    pb_metrics_tick_update(&s_tick, start_us, end_us, scheduled_us);
    if (part_us) pb_metrics_tick_add_parts(&s_tick, part_us);
    taskEXIT_CRITICAL(&s_mux);
}

void pb_metrics_get_tick_stats(pb_metrics_tick_state_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_mux);
    *out = s_tick;
    taskEXIT_CRITICAL(&s_mux);
}

void pb_metrics_airflow_tick(int64_t now_us, bool heat, bool relay, bool fan_on,
                             int32_t ptc_dc, int32_t chamber_dc)
{
    taskENTER_CRITICAL(&s_mux);
    if (!s_airflow_init) {
        pb_airflow_init(&s_airflow);
        s_airflow_init = true;
    }
    pb_airflow_update(&s_airflow, now_us, heat, relay, fan_on, ptc_dc, chamber_dc);
    taskEXIT_CRITICAL(&s_mux);
}

void pb_metrics_get_airflow(pb_airflow_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_mux);
    if (s_airflow_init) *out = s_airflow;
    else pb_airflow_init(out);
    taskEXIT_CRITICAL(&s_mux);
}

static void alloc_failed(size_t size, uint32_t caps, const char *function_name)
{
    (void)caps;
    (void)function_name;
    __atomic_fetch_add(&s_alloc_failures, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_alloc_failure_last, (uint32_t)size, __ATOMIC_RELAXED);
}

void pb_metrics_get_alloc_failures(uint32_t *count, uint32_t *last_bytes)
{
    if (count) *count = __atomic_load_n(&s_alloc_failures, __ATOMIC_RELAXED);
    if (last_bytes) *last_bytes = __atomic_load_n(&s_alloc_failure_last, __ATOMIC_RELAXED);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const wifi_event_sta_disconnected_t *d = data;
    __atomic_fetch_add(&s_wifi_disconnects, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_wifi_last_reason, d ? (uint32_t)d->reason : 0u, __ATOMIC_RELAXED);
}

void pb_metrics_net_init(void)
{
    esp_err_t e = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                             wifi_event, NULL);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "Wi-Fi disconnect counter: %s", esp_err_to_name(e));
}

void pb_metrics_get_wifi_disconnects(uint32_t *count, uint16_t *last_reason)
{
    if (count) *count = __atomic_load_n(&s_wifi_disconnects, __ATOMIC_RELAXED);
    if (last_reason) {
        uint32_t r = __atomic_load_n(&s_wifi_last_reason, __ATOMIC_RELAXED);
        *last_reason = r > UINT16_MAX ? UINT16_MAX : (uint16_t)r;
    }
}

static void take_panic(nvs_handle_t h, bool nvs_ok)
{
    bool first = false;
    s_last_panic_valid = pb_panic_take(&s_last_panic, &first);
    uint32_t n = 0;
    if (nvs_ok) {
        (void)nvs_get_u32(h, KEY_PANIC_CNT, &n);   // not found: n stays 0
        if (first) {
            n++;
            nvs_ok = nvs_set_u32(h, KEY_PANIC_CNT, n) == ESP_OK;
        }
    }
    s_panic_count = n;
    s_panic_count_valid = nvs_ok;
    if (!s_last_panic_valid) return;
    char bt[PB_PANIC_BT_TEXT + 1];
    pb_panic_bt_text(&s_last_panic, bt, sizeof bt);
    ESP_LOGW(TAG, "%s panic: task=%s kind=%s pc=0x%08lx ra=0x%08lx mcause=0x%lx mtval=0x%lx "
             "reason=\"%s\" bt=%s",
             first ? "last" : "earlier", s_last_panic.task,
             pb_panic_kind_str(s_last_panic.kind),
             (unsigned long)s_last_panic.pc, (unsigned long)s_last_panic.ra,
             (unsigned long)s_last_panic.mcause, (unsigned long)s_last_panic.mtval,
             s_last_panic.reason, bt[0] ? bt : "-");
}

void pb_metrics_boot_init(void)
{
    s_reset_reason = (unsigned)esp_reset_reason();
    if (heap_caps_register_failed_alloc_callback(alloc_failed) != ESP_OK)
        ESP_LOGW(TAG, "failed-allocation callback not registered");

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "boot count: nvs_open failed, reset reason %u", s_reset_reason);
        take_panic(0, false);
        return;
    }
    uint32_t n = 0;
    (void)nvs_get_u32(h, KEY_BOOT_CNT, &n);   // not found: first boot, n stays 0
    n++;
    bool ok = nvs_set_u32(h, KEY_BOOT_CNT, n) == ESP_OK;
    take_panic(h, true);
    if (ok && nvs_commit(h) == ESP_OK) {
        s_boot_count = n;
        ESP_LOGI(TAG, "boot %lu, reset reason %u", (unsigned long)n, s_reset_reason);
    } else {
        s_panic_count_valid = false;
        ESP_LOGW(TAG, "boot count: NVS write failed, reset reason %u", s_reset_reason);
    }
    nvs_close(h);
}

uint32_t pb_metrics_boot_count(void) { return s_boot_count; }
unsigned pb_metrics_reset_reason(void) { return s_reset_reason; }

bool pb_metrics_panic_count(uint32_t *count)
{
    if (count) *count = s_panic_count;
    return s_panic_count_valid;
}

bool pb_metrics_last_panic(pb_panic_record_t *out)
{
    if (s_last_panic_valid && out) *out = s_last_panic;
    return s_last_panic_valid;
}
