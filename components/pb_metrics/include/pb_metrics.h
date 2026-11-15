// SPDX-License-Identifier: MIT
// pb_metrics — device telemetry for GET /metrics: control-tick timing, boot
// count, reset reason, panic breadcrumb, failed allocations, Wi-Fi disconnects
// and airflow signs. The pure parts are in pb_metrics_fmt.h,
// pb_metrics_airflow.h and pb_panic_record.h.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pb_metrics_airflow.h"
#include "pb_metrics_fmt.h"
#include "pb_panic_record.h"

// Control task only. Record one loop iteration (see pb_metrics_tick_update)
// and, if part_us is not NULL, the time of each part (PB_METRICS_PART_*).
// No logging and no blocking: one short critical section.
void pb_metrics_tick_record(int64_t start_us, int64_t end_us, int64_t scheduled_us,
                            const uint32_t part_us[PB_METRICS_TICK_PARTS]);

void pb_metrics_get_tick_stats(pb_metrics_tick_state_t *out);

// Control task only, each tick. See pb_airflow_update(). O(1), integer math,
// one short critical section.
void pb_metrics_airflow_tick(int64_t now_us, bool heat, bool relay, bool fan_on,
                             int32_t ptc_dc, int32_t chamber_dc);
void pb_metrics_get_airflow(pb_airflow_t *out);

// Call once at boot, after nvs_flash_init(). Reads esp_reset_reason(),
// increments the boot count in NVS (app_nvs/"boot_cnt"), registers the
// failed-allocation callback, and takes the panic breadcrumb: if a new one is
// there, it logs it and increments app_nvs/"panic_cnt".
void pb_metrics_boot_init(void);

// Call after the default event loop exists (after dc_wifi_start). Counts the
// Wi-Fi station disconnect events.
void pb_metrics_net_init(void);

uint32_t pb_metrics_boot_count(void);     // 0 if NVS failed (count unknown)
unsigned pb_metrics_reset_reason(void);   // esp_reset_reason_t of this boot

void pb_metrics_get_alloc_failures(uint32_t *count, uint32_t *last_bytes);
void pb_metrics_get_wifi_disconnects(uint32_t *count, uint16_t *last_reason);

// The panic count from NVS (false if NVS failed) and the last breadcrumb
// (false if there is none).
bool pb_metrics_panic_count(uint32_t *count);
bool pb_metrics_last_panic(pb_panic_record_t *out);
