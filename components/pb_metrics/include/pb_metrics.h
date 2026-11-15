// SPDX-License-Identifier: MIT
// pb_metrics — device telemetry for GET /metrics: control-tick timing, boot
// count and reset reason. The pure parts are in pb_metrics_fmt.h.
#pragma once

#include <stdint.h>

#include "pb_metrics_fmt.h"

// Control task only. Record one loop iteration (see pb_metrics_tick_update).
// No logging and no blocking: one short critical section.
void pb_metrics_tick_record(int64_t start_us, int64_t end_us, int64_t scheduled_us);

void pb_metrics_get_tick_stats(pb_metrics_tick_state_t *out);

// Call once at boot, after nvs_flash_init(). Reads esp_reset_reason() and
// increments the boot count in NVS (app_nvs/"boot_cnt").
void pb_metrics_boot_init(void);

uint32_t pb_metrics_boot_count(void);     // 0 if NVS failed (count unknown)
unsigned pb_metrics_reset_reason(void);   // esp_reset_reason_t of this boot
