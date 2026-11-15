// SPDX-License-Identifier: MIT
// pb_metrics_fmt — the pure parts of GET /metrics:
//   - a Prometheus text-format (0.0.4) writer that streams through a small
//     caller buffer and a sink callback
//   - control-tick statistics, temperature conversion and task aggregation
//
// No ESP-IDF headers, no printf, no malloc. The writer uses integer math only,
// so it is cheap on the ESP32-C3 (no FPU). All of it compiles on the host.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pb_panic_record.h"

#define PB_METRICS_MAX_TASKS      24   // distinct task names in one response
#define PB_METRICS_TASK_NAME_LEN  16   // >= configMAX_TASK_NAME_LEN - 1
// A temperature or target without a valid value. The writer omits that sample.
#define PB_METRICS_TEMP_UNKNOWN   INT32_MIN
// A periodic control tick is late if it starts more than this after its schedule.
#define PB_METRICS_TICK_LATE_US   250000

// Fault causes. The index is the pb_fault_reason_t code of pb_heater.h.
// pb_httpd_metrics.c static-asserts each value against that enum.
#define PB_METRICS_CAUSE_NONE              0
#define PB_METRICS_CAUSE_PTC_OVERTEMP      1
#define PB_METRICS_CAUSE_CHAMBER_OVERTEMP  2
#define PB_METRICS_CAUSE_CHAMBER_SENSOR    3
#define PB_METRICS_CAUSE_PTC_SENSOR        4
#define PB_METRICS_CAUSE_LINK_LOST         5
#define PB_METRICS_CAUSE_PANIC_OFF         6   // own counter: panic_offs
#define PB_METRICS_CAUSE_INHIBITED         7
#define PB_METRICS_CAUSE_EMERGENCY         8
#define PB_METRICS_CAUSE_NVS_UNREADABLE    9   // boot restore only, never counted
#define PB_METRICS_CAUSE_HEATER_NO_RISE   10   // first-heat check
#define PB_METRICS_CAUSE_SENSOR_ROLES     11   // first-heat check
#define PB_METRICS_FAULT_CAUSES           12

// Control-tick duration histogram: upper bounds in microseconds. The +Inf
// bucket is the tick count.
#define PB_METRICS_TICK_BUCKETS 8
#define PB_METRICS_TICK_BUCKET_BOUNDS_US \
    { 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000 }

// Parts of a control tick (dragonbreath_control_tick_part_seconds_total).
#define PB_METRICS_PART_SOURCE   0   // read the control source, feed the policy
#define PB_METRICS_PART_SENSORS  1   // pb_ntc reads in pb_heater_tick
#define PB_METRICS_PART_POLICY   2   // pb_policy_tick without pb_heater_tick
#define PB_METRICS_PART_HEATER   3   // pb_heater_tick without the sensor reads
#define PB_METRICS_PART_OTHER    4   // the rest (watchdog, status sample, metrics)
#define PB_METRICS_TICK_PARTS    5

// PrusaLink states. The values are dc_prusa_state_t.
#define PB_METRICS_PRUSA_STATES  5

// Number of esp_reset_reason_t values with a name (ESP_RST_UNKNOWN..CPU_LOCKUP).
// pb_metrics.c static-asserts it against ESP_RST_CPU_LOCKUP.
#define PB_METRICS_RESET_REASONS          16

typedef struct {
    char name[PB_METRICS_TASK_NAME_LEN + 1];
    uint16_t instances;             // tasks with this name
    uint32_t stack_free_min_bytes;  // smallest high-water mark of these tasks (bytes)
    uint64_t cpu_us;                // sum of their run-time counters (1 us clock)
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
    uint8_t fault_code;             // pb_fault_reason_t (PB_METRICS_CAUSE_*)
    bool source_connected;

    // Counters since boot.
    uint32_t trips[PB_METRICS_FAULT_CAUSES];  // trips by cause, see pb_heater_counters_t
    uint32_t panic_offs;
    uint64_t relay_on_ms;
    uint32_t ticks;
    uint32_t ticks_late;

    // Control-tick timing since boot (microseconds, written as seconds).
    uint32_t tick_last_us;
    uint32_t tick_max_us;
    uint32_t tick_interval_max_us;
    uint32_t tick_bucket[PB_METRICS_TICK_BUCKETS];   // ticks in each bucket (not cumulative)
    uint64_t tick_sum_us;
    uint64_t tick_part_us[PB_METRICS_TICK_PARTS];
    bool tick_parts_valid;

    // Heap allocations that failed (heap_caps failed-alloc callback).
    uint32_t alloc_failures;
    uint32_t alloc_failure_last_bytes;

    // Wi-Fi station disconnect events since boot, and the reason of the last one
    // (wifi_err_reason_t; 0 before the first).
    uint32_t wifi_disconnects;
    uint16_t wifi_last_reason;

    // PrusaLink client (only if PrusaLink is the control source).
    bool prusa_valid;
    uint8_t prusa_state;            // dc_prusa_state_t
    uint32_t prusa_status_age_ms;   // UINT32_MAX: no status yet, sample omitted

    // Panic breadcrumb.
    uint32_t panics;                // panics counted at boot, kept in NVS; 0 also if unknown
    bool panics_valid;              // false: NVS failed, sample omitted
    bool last_panic_valid;
    pb_panic_record_t last_panic;

    // Airflow signs (report-only), see pb_metrics_airflow.h.
    int32_t airflow_rise_permille;  // PB_METRICS_TEMP_UNKNOWN: sample omitted
    uint32_t airflow_rise_samples;
    uint32_t airflow_cool_tau_ms;   // 0: sample omitted
    uint32_t airflow_cool_samples;
    uint32_t foldback_cuts;

    // Heap (bytes).
    uint32_t heap_free;
    uint32_t heap_min_free;
    uint32_t heap_largest_block;

    // Tasks, one entry for each task name (see pb_metrics_add_task). task_count
    // is the number of tasks in the system. The families are written only if
    // the firmware has the data (trace facility, run-time stats).
    bool task_stack_valid;
    bool task_cpu_valid;
    uint16_t task_count;
    uint16_t n_tasks;
    pb_metrics_task_t tasks[PB_METRICS_MAX_TASKS];

    // Device.
    uint64_t uptime_ms;
    uint32_t boot_count;            // 0: unknown (NVS failed), sample omitted
    uint8_t reset_reason;           // esp_reset_reason_t
    const char *version;            // firmware version (label value, escaped)
    const char *idf_version;        // ESP-IDF version (label value, escaped)
    bool rssi_valid;
    int8_t rssi_dbm;
    int16_t http_sockets;           // < 0: unknown, sample omitted

    // Syslog and wall clock (pb_netlog). All 0 if syslog is off.
    bool syslog_enabled;            // dc_syslog runs
    uint32_t syslog_sent;           // datagrams sent since boot
    uint32_t syslog_dropped;        // lines dropped, queue full
    uint32_t syslog_send_errors;    // failed sends and address lookups
    uint32_t syslog_queued;         // lines in the queue now
    bool time_synced;               // SNTP synced at least once (logs only)
} pb_metrics_snapshot_t;

// Receives each full buffer and the last part. Returns false to stop the writer
// (for example when the client closed the socket).
typedef bool (*pb_metrics_sink_fn)(void *ctx, const char *data, size_t len);

// Write the snapshot as Prometheus text format 0.0.4. The writer fills buf (cap
// bytes, any size >= 1) and gives it to sink when it is full and at the end.
// Returns the total number of bytes, or -1 if an argument is NULL, cap is 0,
// or the sink returned false. The text is not NUL-terminated.
long pb_metrics_format(const pb_metrics_snapshot_t *s, char *buf, size_t cap,
                       pb_metrics_sink_fn sink, void *ctx);

// Stable snake_case name for an esp_reset_reason_t value ("unknown" if out of range).
const char *pb_metrics_reset_reason_str(unsigned reason);

// Add one task to s->tasks. Tasks with the same name share one entry: the CPU
// time is the sum and the stack value is the minimum. The entry order does not
// change the output values. Returns false if a new name has no free slot.
bool pb_metrics_add_task(pb_metrics_snapshot_t *s, const char *name,
                         uint32_t stack_free_bytes, uint64_t cpu_us);

// Degrees C to tenths, rounded half away from zero. A value that is not finite
// or out of +-100000 C gives PB_METRICS_TEMP_UNKNOWN.
int32_t pb_metrics_tenths(float c);

// Control-tick statistics since boot.
typedef struct {
    uint32_t ticks;             // control loop iterations
    uint32_t late;              // periodic ticks that started late
    uint32_t last_us;           // duration of the last tick
    uint32_t max_us;            // longest tick
    uint32_t interval_max_us;   // longest time between two tick starts
    int64_t prev_start_us;      // start of the previous tick; 0 before the first
    uint32_t bucket[PB_METRICS_TICK_BUCKETS];   // ticks in each bucket (not cumulative)
    uint64_t sum_us;            // sum of all tick durations
    uint64_t part_us[PB_METRICS_TICK_PARTS];
    bool parts_valid;           // true after the first pb_metrics_tick_add_parts()
} pb_metrics_tick_state_t;

// Record one loop iteration. scheduled_us is the planned start of a periodic
// tick, or 0 for a wake by notification (never late). Durations clamp to
// 0..UINT32_MAX us.
void pb_metrics_tick_update(pb_metrics_tick_state_t *st, int64_t start_us,
                            int64_t end_us, int64_t scheduled_us);

// Add the time of each part of one tick (microseconds) to the part counters.
void pb_metrics_tick_add_parts(pb_metrics_tick_state_t *st,
                               const uint32_t part_us[PB_METRICS_TICK_PARTS]);

// Split the measured times of one tick into the parts (all in us). Each
// argument is a span of the tick: tick (whole), source (before pb_policy_tick),
// policy_tick (pb_policy_tick), heater_tick and sensors (inside it). A part
// never goes below 0, and the parts sum to tick if the spans nest.
void pb_metrics_tick_split(uint32_t tick_us, uint32_t source_us, uint32_t policy_tick_us,
                           uint32_t heater_tick_us, uint32_t sensors_us,
                           uint32_t part_us[PB_METRICS_TICK_PARTS]);
