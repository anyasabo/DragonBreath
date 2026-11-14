// SPDX-License-Identifier: MIT
// pb_metrics — device telemetry for GET /metrics: control-tick timing, boot
// count and reset reason. The text writer is in pb_metrics_fmt.h.
#pragma once

#include <stdint.h>

// A tick is late if it starts more than this time after its scheduled time.
#define PB_METRICS_TICK_LATE_US  250000

typedef struct {
    uint32_t ticks;             // control loop iterations since boot
    uint32_t late;              // periodic ticks that started late
    uint32_t last_us;           // duration of the last tick
    uint32_t max_us;            // longest tick since boot
    uint32_t interval_max_us;   // longest time between two tick starts
} pb_metrics_tick_stats_t;

// Control task only. Record one loop iteration. scheduled_us is the planned
// start of a periodic tick, or 0 for a wake by notification (never late).
// No logging and no blocking: one short critical section.
void pb_metrics_tick_record(int64_t start_us, int64_t end_us, int64_t scheduled_us);

void pb_metrics_get_tick_stats(pb_metrics_tick_stats_t *out);

// Call once at boot, after nvs_flash_init(). Reads esp_reset_reason() and
// increments the boot count in NVS (app_nvs/"boot_cnt").
void pb_metrics_boot_init(void);

uint32_t pb_metrics_boot_count(void);
unsigned pb_metrics_reset_reason(void);   // esp_reset_reason_t of this boot
