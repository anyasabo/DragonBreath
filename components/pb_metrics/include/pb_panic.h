// SPDX-License-Identifier: MIT
// pb_panic — the panic breadcrumb hook (see pb_panic.c and pb_panic_record.h).
#pragma once

#include <stdbool.h>

#include "pb_panic_record.h"

// Call once at boot. If RTC RAM holds a valid record, copy it to out and return
// true. *first_report is true until pb_panic_mark_reported() (count it), and
// false after (a soft reset keeps the record). The record stays in RTC RAM
// until a new panic or a power cycle.
bool pb_panic_take(pb_panic_record_t *out, bool *first_report);

// Call after the NVS commit of the panic count. If the commit fails, do not
// call it: the next boot counts the panic again.
void pb_panic_mark_reported(void);
