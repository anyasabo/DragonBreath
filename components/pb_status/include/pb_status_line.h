// SPDX-License-Identifier: MIT
// pb_status_line — the pure part of the periodic status line: format a sample
// with integer math and collapse repeated lines. No ESP-IDF headers, no float
// formatting. It compiles on the host.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A temperature without a valid value. The line shows "nan", as "%.1f" did.
#define PB_STATUS_UNKNOWN   INT32_MIN
#define PB_STATUS_LINE_MAX  224
// A liveness line each N repeated samples (~60 s at one sample each 2 s).
#define PB_STATUS_REPEAT_FLUSH 30

// One temperature: tenths for the line, whole degrees for the key. Both are
// rounded from the float, so a value near a tie gives the same text as the old
// "%.1f" and "%.0f" (one rounding, not two).
typedef struct {
    int32_t dc;                 // tenths of a degree C, or PB_STATUS_UNKNOWN
    int32_t whole;              // degrees C, or PB_STATUS_UNKNOWN
} pb_status_temp_t;

// One status sample. The control task fills it and the status task formats it.
// The const char * fields must point to string literals (for example the
// results of pb_policy_mode_str()), because the sample crosses tasks.
typedef struct {
    uint32_t revision;
    const char *mode;
    const char *source;
    pb_status_temp_t target;    // shown in whole degrees
    bool heater_on;
    pb_status_temp_t chamber;
    pb_status_temp_t ptc;
    int wifi;                   // dc_wifi state, -1 before the network starts
    bool bambu;                 // true: the Bambu form of the line
    int link;                   // Bambu: dc_bambu state. Other: Moonraker state.
    // Bambu only.
    bool printing;
    char filament[16];          // "" shows as "-"
    pb_status_temp_t bed;       // also used by the other form
    pb_status_temp_t bed_target;
    pb_status_temp_t chsrc;
    // Other sources only.
    const char *src;            // dc_source_str()
    const char *printer;        // dc_printer_state_str()
    // Zero-cross diagnostics: they change on each sample, so not in the key.
    uint32_t zc_n;
    uint32_t zc_dt_us;
    uint32_t zc_rejected;
} pb_status_sample_t;

typedef struct {
    char last_key[PB_STATUS_LINE_MAX];
    uint32_t repeats;
} pb_status_dedup_t;

typedef void (*pb_status_emit_fn)(void *ctx, const char *line);

// Degrees C to tenths and to whole degrees, each rounded half away from zero.
// Not finite, or out of +-100000 C: PB_STATUS_UNKNOWN. Float math only.
pb_status_temp_t pb_status_temp(float c);

// Write the line ("rev=... | chamber=21.5C ...") without the ZC part. If
// whole is true, temperatures are whole degrees (the key that finds repeats).
void pb_status_format(const pb_status_sample_t *s, bool whole, char *buf, size_t cap);

// Feed one sample. emit gets 0, 1 or 2 complete log lines, in this order:
// "(previous line repeated Nx)" when the line changes after repeats, then the
// new line with its ZC part. A repeated line gives no output, except each
// PB_STATUS_REPEAT_FLUSH repeats ("... | repeated Nx | ZC ...").
void pb_status_feed(pb_status_dedup_t *st, const pb_status_sample_t *s,
                    pb_status_emit_fn emit, void *ctx);
