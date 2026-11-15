// SPDX-License-Identifier: MIT
#pragma once
// Parse and check LAN input for pb_httpd.c before any of it reaches a setter.
// Pure functions: the host test tests/pb_httpd_input_host_test.c uses them.

#include "esp_http_server.h"
#include "pb_policy.h"
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Parse a finite decimal temperature. Trailing non-whitespace is rejected so a
// partially numeric value can never silently become a safety setting.
static inline bool pb_httpd_parse_temp(const char *s, float *out)
{
    if (!s || !*s) return false;
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s || !isfinite(v)) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end != '\0') return false;
    *out = v;
    return true;
}

// Parse a decimal integer in 0..UINT32_MAX. The first character must be a
// digit. Thus a sign ("-1", "+1") and leading whitespace are bad. A value
// above UINT32_MAX is bad. strtoul() alone accepts "-1" as ULONG_MAX, and
// unsigned long has 64 bits on a host but 32 bits on the ESP32-C3. strtoull()
// has 64 bits on both, so the host test checks the device behaviour.
static inline bool pb_httpd_parse_u32(const char *s, uint32_t *out)
{
    if (!s || *s < '0' || *s > '9') return false;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno == ERANGE || v > UINT32_MAX) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end != '\0') return false;
    *out = (uint32_t)v;
    return true;
}

// The fields of one POST /settings request.
typedef struct {
    bool has_max, has_comms_ms, has_cool_release, has_fb_cut;
    bool has_filter_temp, has_filter_auto, has_leds_enabled, has_swap;
    bool has_first_heat_rerun, has_first_heat_apply_swap;
    float max, cool_release, fb_cut, filter_temp;
    uint32_t comms_ms;
    bool filter_auto, leds_enabled, swap;
} pb_httpd_settings_t;

// Parse and check every field of a /settings query. Return NULL if all fields
// are good, or else the message for HTTP 400. This function applies nothing, so
// a request with one bad field changes no setting.
static inline const char *pb_httpd_settings_parse(const char *q, pb_httpd_settings_t *s)
{
    memset(s, 0, sizeof *s);
    char v[24];
    uint32_t u;
    if (httpd_query_key_value(q, "max", v, sizeof v) == ESP_OK) {
        if (!pb_httpd_parse_temp(v, &s->max)) return "bad max";
        s->has_max = true;
    }
    if (httpd_query_key_value(q, "comms_ms", v, sizeof v) == ESP_OK) {
        if (!pb_httpd_parse_u32(v, &s->comms_ms)) return "bad comms_ms";
        s->has_comms_ms = true;
    }
    if (httpd_query_key_value(q, "cool_release", v, sizeof v) == ESP_OK) {
        if (!pb_httpd_parse_temp(v, &s->cool_release)) return "bad cool_release";
        s->has_cool_release = true;
    }
    if (httpd_query_key_value(q, "fb_cut", v, sizeof v) == ESP_OK) {
        if (!pb_httpd_parse_temp(v, &s->fb_cut)) return "bad fb_cut";
        s->has_fb_cut = true;
    }
    if (httpd_query_key_value(q, "filter_temp", v, sizeof v) == ESP_OK) {
        if (!pb_httpd_parse_temp(v, &s->filter_temp)) return "bad filter_temp";
        // The same range as pb_policy_set_filter_config().
        if (s->filter_temp < PB_POLICY_FILTER_TEMP_MIN_C
                || s->filter_temp > PB_POLICY_FILTER_TEMP_MAX_C)
            return "filter_temp out of range";
        s->has_filter_temp = true;
    }
    if (httpd_query_key_value(q, "filter_auto", v, sizeof v) == ESP_OK) {
        if (!pb_httpd_parse_u32(v, &u)) return "bad filter_auto";
        s->filter_auto = u != 0;
        s->has_filter_auto = true;
    }
    if (httpd_query_key_value(q, "leds_enabled", v, sizeof v) == ESP_OK) {
        if (!pb_httpd_parse_u32(v, &u)) return "bad leds_enabled";
        s->leds_enabled = u != 0;
        s->has_leds_enabled = true;
    }
    if (httpd_query_key_value(q, "swap_thermistors", v, sizeof v) == ESP_OK) {
        if (!pb_httpd_parse_u32(v, &u) || u > 1) return "bad swap_thermistors";
        s->swap = u != 0;
        s->has_swap = true;
    }
    // First-heat actions: the only good value is 1.
    if (httpd_query_key_value(q, "first_heat_apply_swap", v, sizeof v) == ESP_OK) {
        if (!pb_httpd_parse_u32(v, &u) || u != 1) return "bad first_heat_apply_swap";
        s->has_first_heat_apply_swap = true;
    }
    if (httpd_query_key_value(q, "first_heat_rerun", v, sizeof v) == ESP_OK) {
        if (!pb_httpd_parse_u32(v, &u) || u != 1) return "bad first_heat_rerun";
        s->has_first_heat_rerun = true;
    }
    // Both fields write the staged thermistor mapping. One request must not set it twice.
    if (s->has_swap && s->has_first_heat_apply_swap)
        return "swap_thermistors and first_heat_apply_swap together";
    return NULL;
}

// Convert a finite JSON zone target to the uint8_t of dc_bambu_zone_set(),
// which clamps it to 70. A cast of a value above 255 is undefined behaviour:
// on the ESP32-C3 it wraps, so 300 became 44 and 256 became 0 (zone off).
static inline uint8_t pb_httpd_zone_target_c(double t)
{
    if (t < 0) return 0;
    if (t > UINT8_MAX) return UINT8_MAX;
    return (uint8_t)t;
}
