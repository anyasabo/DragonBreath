// SPDX-License-Identifier: MIT
// pb_metrics_fmt — pure Prometheus text-format (0.0.4) writer for GET /metrics.
//
// The caller fills a plain snapshot struct and gives a buffer with a fixed size.
// The writer uses integer math only (no float, no printf, no malloc), so it is
// cheap on the ESP32-C3 (no FPU) and it compiles on the host for tests.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PB_METRICS_MAX_TASKS      24
#define PB_METRICS_TASK_NAME_LEN  20   // FreeRTOS name (16) + "#NN" duplicate suffix
// A temperature or target without a valid value. The writer omits that sample.
#define PB_METRICS_TEMP_UNKNOWN   INT32_MIN
// Response buffer for GET /metrics. The host test formats a worst-case snapshot
// (all tasks, longest names, maximum values) and checks that it fits with margin.
#define PB_METRICS_BUF_SIZE       15360

// Fault-cause slots. The index is the pb_fault_reason_t code of pb_heater.h.
#define PB_METRICS_FAULT_CAUSES   10
#define PB_METRICS_CAUSE_PANIC_OFF       6   // own counter: panic_offs
#define PB_METRICS_CAUSE_NVS_UNREADABLE  9   // boot restore only, never counted

typedef struct {
    char name[PB_METRICS_TASK_NAME_LEN + 1];
    uint32_t stack_free_min_bytes;  // FreeRTOS high-water mark (bytes on ESP-IDF)
    uint64_t cpu_us;                // run-time stats counter (1 us ESP timer clock)
} pb_metrics_task_t;

typedef struct {
    // Sensors. Temperatures are tenths of a degree C (-52 = -5.2 C).
    int32_t chamber_dc;             // PB_METRICS_TEMP_UNKNOWN: omit the sample
    int32_t ptc_dc;
    uint8_t chamber_status;         // pb_ntc_status_t: 0 ok, 1 short, 2 open, 3 uninit
    uint8_t ptc_status;

    // Control state.
    bool heater_demand;
    bool relay_on;
    uint8_t fan_percent;
    uint8_t mode;                   // pb_mode_t: 0 off, 1 power_on, 2 auto, 3 drying
    int32_t target_dc;              // effective target, tenths of a degree C
    int32_t zone_target_dc;         // filament-zone target; UNKNOWN when no zone
    bool fault_latched;
    bool inhibited;
    uint8_t fault_code;             // pb_fault_reason_t
    bool source_connected;

    // Counters since boot.
    uint32_t trips[PB_METRICS_FAULT_CAUSES];  // new latch episodes by cause
    uint32_t panic_offs;
    uint64_t relay_on_ms;
    uint32_t ticks;
    uint32_t ticks_late;

    // Control-tick timing since boot (microseconds).
    uint32_t tick_last_us;
    uint32_t tick_max_us;
    uint32_t tick_interval_max_us;

    // Heap (bytes).
    uint32_t heap_free;
    uint32_t heap_min_free;
    uint32_t heap_largest_block;

    // Tasks. task_count is the number of tasks in the system. It can be larger
    // than n_tasks (the number of entries in tasks[]).
    uint16_t task_count;
    uint16_t n_tasks;
    pb_metrics_task_t tasks[PB_METRICS_MAX_TASKS];

    // Device.
    uint64_t uptime_ms;
    uint32_t boot_count;
    uint8_t reset_reason;           // esp_reset_reason_t
    const char *version;            // firmware version (label value, escaped)
    const char *idf_version;        // ESP-IDF version (label value, escaped)
    bool rssi_valid;
    int8_t rssi_dbm;
    int16_t http_sockets;           // < 0: unknown, sample omitted
    uint32_t last_build_us;         // build time of the previous /metrics response
} pb_metrics_snapshot_t;

// Write the snapshot as Prometheus text format 0.0.4 into buf (NUL-terminated).
// Returns the length without the NUL, or -1 if the text does not fit (or an
// argument is NULL). On -1 the buffer content is not a valid response.
int pb_metrics_format(const pb_metrics_snapshot_t *s, char *buf, size_t cap);

// Stable snake_case name for an esp_reset_reason_t value ("unknown" if out of range).
const char *pb_metrics_reset_reason_str(unsigned reason);
