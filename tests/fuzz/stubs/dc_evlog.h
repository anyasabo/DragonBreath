#pragma once
// The parts of dragon-core v0.36.0 dc_evlog.h that the handlers use.
#include <stddef.h>
#include <stdint.h>

#define DC_EVLOG_MAX_ENTRIES   64
#define DC_EVLOG_TEXT_BYTES    96
#define DC_EVLOG_CONSOLE_BYTES 16384

typedef struct {
    uint32_t ms;
    char text[DC_EVLOG_TEXT_BYTES];
} dc_evlog_entry_t;

void dc_evlog_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
size_t dc_evlog_snapshot(dc_evlog_entry_t *out, size_t max);
size_t dc_evlog_console_snapshot(char *out, size_t max);
