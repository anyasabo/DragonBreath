// SPDX-License-Identifier: MIT
// pb_status — the periodic status log line, out of the control task.
//
// The control task copies a small sample (pb_status_submit: a short critical
// section and a task notification, no formatting, no logging, no blocking).
// A low-priority task formats it with integer math and logs it.
#pragma once

#include "esp_err.h"
#include "pb_status_line.h"

// Start the status task. Before the start, pb_status_submit() does nothing.
esp_err_t pb_status_start(void);

// Control task. Copy the sample and wake the status task. If the status task
// has not taken the previous sample yet, the new sample replaces it.
void pb_status_submit(const pb_status_sample_t *s);
