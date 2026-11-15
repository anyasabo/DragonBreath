// Host test for pb_httpd_input.h: POST /settings checks every field before it
// applies one, and a JSON zone target above 255 does not wrap.
#include "pb_httpd_input.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

#define ESP_ERR_NOT_FOUND 0x105

// The logic of ESP-IDF v5.3 httpd_query_key_value() (httpd_parse.c).
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size)
{
    const char *p = qry;
    while (strlen(p)) {
        const char *eq = strchr(p, '=');
        if (!eq) break;
        size_t offset = (size_t)(eq - p);
        if (offset != strlen(key) || strncasecmp(p, key, offset)) {
            p = strchr(eq, '&');
            if (!p) break;
            p++;
            continue;
        }
        const char *v = eq + 1;
        const char *end = strchr(v, '&');
        if (!end) end = v + strlen(v);
        size_t need = (size_t)(end - v) + 1;
        size_t n = (need < val_size ? need : val_size) - 1;
        memcpy(val, v, n);
        val[n] = '\0';
        return val_size < need ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

int main(void)
{
    pb_httpd_settings_t s;

    // All fields good.
    CHECK(pb_httpd_settings_parse("max=60&comms_ms=30000&cool_release=45&fb_cut=0"
                                  "&filter_temp=35&filter_auto=1&leds_enabled=0&swap_thermistors=1", &s) == NULL);
    CHECK(s.has_max && s.max == 60.0f && s.has_comms_ms && s.comms_ms == 30000);
    CHECK(s.has_cool_release && s.has_fb_cut && s.fb_cut == 0.0f);
    CHECK(s.has_filter_temp && s.filter_temp == 35.0f && s.has_filter_auto && s.filter_auto);
    CHECK(s.has_leds_enabled && !s.leds_enabled && s.has_swap && s.swap);

    // A bad field after a good one: the parse fails, so the handler applies
    // nothing. Before the fix, max=60 was applied and the response was 400.
    CHECK(strcmp(pb_httpd_settings_parse("max=60&comms_ms=abc", &s), "bad comms_ms") == 0);
    CHECK(strcmp(pb_httpd_settings_parse("max=60&cool_release=1e99", &s), "bad cool_release") == 0);
    CHECK(strcmp(pb_httpd_settings_parse("max=60&swap_thermistors=2", &s), "bad swap_thermistors") == 0);
    // pb_policy_set_filter_config() rejects this range after the other setters.
    CHECK(strcmp(pb_httpd_settings_parse("max=60&filter_temp=99", &s), "filter_temp out of range") == 0);
    CHECK(strcmp(pb_httpd_settings_parse("max=60&filter_temp=19.9", &s), "filter_temp out of range") == 0);
    CHECK(strcmp(pb_httpd_settings_parse("max=nan", &s), "bad max") == 0);
    CHECK(strcmp(pb_httpd_settings_parse("fb_cut=95x", &s), "bad fb_cut") == 0);

    // No known field.
    CHECK(pb_httpd_settings_parse("other=1", &s) == NULL);
    CHECK(!s.has_max && !s.has_comms_ms && !s.has_filter_temp && !s.has_swap);

    // Zone targets: no wrap above 255 (dc_bambu then clamps to 70).
    CHECK(pb_httpd_zone_target_c(-3.0) == 0);
    CHECK(pb_httpd_zone_target_c(45.9) == 45);
    CHECK(pb_httpd_zone_target_c(256.0) == 255);
    CHECK(pb_httpd_zone_target_c(300.0) == 255);
    CHECK(pb_httpd_zone_target_c(1e300) == 255);

    puts("pb_httpd_input host test: OK");
    return 0;
}
