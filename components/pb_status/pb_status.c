// SPDX-License-Identifier: MIT
// pb_status — the status task. See pb_status.h.
#include "pb_status.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The same tag as the old status line in app_main, so the log stays the same.
static const char *TAG = "dragonbreath";

// The task only formats with integers and logs one line. 3 KB is an estimate:
// newlib printf and the log hooks have no -fstack-usage data. Check the
// high-water mark on the dev board (/metrics task stacks) before a change.
#define PB_STATUS_STACK 3072
#define PB_STATUS_PRIO  1

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static pb_status_sample_t s_pending;      // guarded by s_mux
static TaskHandle_t s_task;
static pb_status_dedup_t s_dedup;         // status task only

static void emit(void *ctx, const char *line)
{
    (void)ctx;
    ESP_LOGI(TAG, "%s", line);
}

static void status_task(void *arg)
{
    (void)arg;
    pb_status_sample_t sample;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        taskENTER_CRITICAL(&s_mux);
        sample = s_pending;
        taskEXIT_CRITICAL(&s_mux);
        pb_status_feed(&s_dedup, &sample, emit, NULL);
    }
}

esp_err_t pb_status_start(void)
{
    if (s_task) return ESP_OK;
    if (xTaskCreate(status_task, "pb_status", PB_STATUS_STACK, NULL,
                    PB_STATUS_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void pb_status_submit(const pb_status_sample_t *s)
{
    if (!s || !s_task) return;
    taskENTER_CRITICAL(&s_mux);
    s_pending = *s;
    taskEXIT_CRITICAL(&s_mux);
    xTaskNotifyGive(s_task);
}
