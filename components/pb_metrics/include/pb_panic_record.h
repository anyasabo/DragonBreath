// SPDX-License-Identifier: MIT
// pb_panic_record — the panic breadcrumb. At a panic, the firmware writes one
// record to RTC no-init RAM (it survives the panic reset, not a power cycle).
// At the next boot, pb_metrics logs it and exports it in GET /metrics.
//
// Pure C (no ESP-IDF headers): the record layout, the CRC, and the text of the
// backtrace. It compiles on the host.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PB_PANIC_MAGIC        0x44425031u   // "DBP1"; a new layout gets a new value
#define PB_PANIC_TASK_LEN     16            // >= configMAX_TASK_NAME_LEN
#define PB_PANIC_REASON_LEN   48
#define PB_PANIC_BT_DEPTH     8             // return addresses (frame pointer builds only)
#define PB_PANIC_BT_TEXT      (PB_PANIC_BT_DEPTH * 11)   // "0x12345678 " each

// The kind of panic. The values are panic_exception_t of ESP-IDF
// (esp_private/panic_internal.h). pb_panic.c static-asserts them.
#define PB_PANIC_KIND_DEBUG   0
#define PB_PANIC_KIND_IWDT    1
#define PB_PANIC_KIND_TWDT    2
#define PB_PANIC_KIND_ABORT   3
#define PB_PANIC_KIND_FAULT   4
#define PB_PANIC_KINDS        5

typedef struct {
    uint32_t magic;
    char task[PB_PANIC_TASK_LEN];       // "" if the task is not known (an ISR at boot)
    uint32_t pc;                        // mepc: the address of the fault
    uint32_t ra;                        // return address register at the fault
    uint32_t sp;
    uint32_t mcause;
    uint32_t mtval;
    uint8_t kind;                       // PB_PANIC_KIND_*
    uint8_t bt_n;                       // valid entries in bt[] (0 without frame pointers)
    uint8_t reported;                   // 1 after a boot counted it
    uint8_t reserved;
    char reason[PB_PANIC_REASON_LEN];   // ESP-IDF reason or abort() details, cut
    uint32_t bt[PB_PANIC_BT_DEPTH];
    uint32_t crc;                       // pb_panic_crc32 of all bytes before this field
} pb_panic_record_t;

// CRC-32 (IEEE, reflected, as zlib). Bitwise: no table in RAM.
uint32_t pb_panic_crc32(const void *data, size_t len);

// Set the magic and the CRC.
void pb_panic_seal(pb_panic_record_t *r);

// True if the magic and the CRC are correct and the fields are in range.
bool pb_panic_valid(const pb_panic_record_t *r);

// Copy a NUL-terminated string into dst (cap >= 1). Only printable ASCII is
// kept (others become '?'), so the text is safe as a log line and a label.
void pb_panic_copy_text(char *dst, size_t cap, const char *src);

// The kind of a panic from what the wrapper of esp_panic_handler sees.
// exception: panic_info_t.exception at the wrapper (before ESP-IDF changes it).
// abort_flag: g_panic_abort. twdt_hint: esp_reset_reason_get_hint() is
// ESP_RST_TASK_WDT.
// On RISC-V (ESP-IDF v5.3 and v6.1), abort() and a task watchdog timeout both
// arrive as PANIC_EXCEPTION_FAULT with g_panic_abort set. The task watchdog
// also sets the reset reason hint first. The interrupt watchdog arrives as
// PANIC_EXCEPTION_IWDT. An unknown value gives PB_PANIC_KIND_FAULT.
uint8_t pb_panic_kind(unsigned exception, bool abort_flag, bool twdt_hint);

// Stable name of a kind: "debug", "int_wdt", "task_wdt", "abort", "fault",
// or "unknown".
const char *pb_panic_kind_str(unsigned kind);

// The backtrace as "0x420012ab 0x42004567", or "" if bt_n is 0. cap >= 1.
void pb_panic_bt_text(const pb_panic_record_t *r, char *buf, size_t cap);
