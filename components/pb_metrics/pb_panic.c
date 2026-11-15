// SPDX-License-Identifier: MIT
// pb_panic — write the panic breadcrumb (pb_panic_record.h) at a panic.
//
// The hook: the linker wraps esp_panic_handler (-Wl,--wrap, see CMakeLists.txt).
// ESP-IDF uses the same wrap in its own components (bt controller, memprot test
// app). esp_panic_handler() runs after the chip panic handler filled
// panic_info_t, with the flash cache enabled again. The wrapper writes a small
// record to RTC no-init RAM and then calls the real handler, so the panic
// output, the core dump and the reboot do not change.
//
// Rules for the wrapper: no locks, no logging, no heap, no FreeRTOS calls that
// can block. Check each pointer before a read. A second entry (a fault inside
// the wrapper) goes directly to the real handler.
//
// Backtrace: on RISC-V without frame pointers (the default, and the only option
// in ESP-IDF v5.3), s0 is a general register, so a frame walk can read random
// memory. The record then has the PC and RA only. With
// CONFIG_ESP_SYSTEM_USE_FRAME_POINTER (ESP-IDF v6.x), the wrapper uses the
// ESP-IDF frame walk (esp_fp_get_callers), which checks each frame pointer.
#include "pb_panic.h"

#include <string.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_memory_utils.h"
#include "esp_private/panic_internal.h"
#include "esp_private/system_internal.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "riscv/rvruntime-frames.h"
#if CONFIG_ESP_SYSTEM_USE_FRAME_POINTER
// ESP-IDF v6.x esp_system/fp_unwind.c. No header declares it: the heap tracer
// (heap/include/heap_trace.inc) declares it the same way. It checks that each
// frame pointer is in data RAM and stops at ROM code.
extern uint32_t esp_fp_get_callers(uint32_t frame, void **callers, void **stacks, uint32_t depth);
#endif

_Static_assert(PANIC_EXCEPTION_DEBUG == PB_PANIC_KIND_DEBUG &&
               PANIC_EXCEPTION_IWDT == PB_PANIC_KIND_IWDT &&
               PANIC_EXCEPTION_TWDT == PB_PANIC_KIND_TWDT &&
               PANIC_EXCEPTION_ABORT == PB_PANIC_KIND_ABORT &&
               PANIC_EXCEPTION_FAULT == PB_PANIC_KIND_FAULT, "panic kind values");
_Static_assert(configMAX_TASK_NAME_LEN <= PB_PANIC_TASK_LEN, "task name length");

static RTC_NOINIT_ATTR pb_panic_record_t s_record;
static bool s_in_wrap;

void __real_esp_panic_handler(panic_info_t *info);

static bool readable(const void *p)
{
    return p && (esp_ptr_byte_accessible(p) || esp_ptr_in_drom(p));
}

static void capture(const panic_info_t *info)
{
    pb_panic_record_t *r = &s_record;
    memset(r, 0, sizeof *r);

    const RvExcFrame *f = readable(info->frame) ? (const RvExcFrame *)info->frame : NULL;
    r->pc = (uint32_t)info->addr;
    if (f) {
        r->ra = f->ra;
        r->sp = f->sp;
        r->mcause = f->mcause;
        r->mtval = f->mtval;
    }
    // The wrapper runs before esp_panic_handler() sets ABORT, so the kind
    // comes from g_panic_abort and the reset reason hint (pb_panic_kind).
    const bool twdt = esp_reset_reason_get_hint() == ESP_RST_TASK_WDT;
    r->kind = pb_panic_kind((unsigned)info->exception, g_panic_abort, twdt);

    // The abort details ("abort() was called at PC ...") are better than the
    // generic reason. The task watchdog sets no details.
    const char *reason = (g_panic_abort && readable(g_panic_abort_details))
        ? g_panic_abort_details
        : twdt ? "Task watchdog got triggered" : info->reason;
    if (readable(reason)) pb_panic_copy_text(r->reason, sizeof r->reason, reason);

    TaskHandle_t t = xTaskGetCurrentTaskHandle();
    if (readable(t)) {
        const char *name = pcTaskGetName(t);
        if (readable(name)) pb_panic_copy_text(r->task, sizeof r->task, name);
    }

#if CONFIG_ESP_SYSTEM_USE_FRAME_POINTER
    if (f) {
        void *callers[PB_PANIC_BT_DEPTH];
        uint32_t n = esp_fp_get_callers(f->s0, callers, NULL, PB_PANIC_BT_DEPTH);
        for (uint32_t i = 0; i < n && i < PB_PANIC_BT_DEPTH; i++)
            r->bt[i] = (uint32_t)callers[i];
        r->bt_n = (uint8_t)(n < PB_PANIC_BT_DEPTH ? n : PB_PANIC_BT_DEPTH);
    }
#endif
    pb_panic_seal(r);
}

void __wrap_esp_panic_handler(panic_info_t *info)
{
    if (!s_in_wrap && info) {
        s_in_wrap = true;
        capture(info);
    }
    __real_esp_panic_handler(info);
}

bool pb_panic_take(pb_panic_record_t *out, bool *first_report)
{
    if (first_report) *first_report = false;
    if (!pb_panic_valid(&s_record)) {
        memset(&s_record, 0, sizeof s_record);   // clear random power-on content
        return false;
    }
    if (first_report) *first_report = !s_record.reported;
    if (out) *out = s_record;
    return true;
}

void pb_panic_mark_reported(void)
{
    if (pb_panic_valid(&s_record) && !s_record.reported) {
        s_record.reported = 1;
        pb_panic_seal(&s_record);
    }
}
