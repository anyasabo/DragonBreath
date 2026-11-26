// SPDX-License-Identifier: MIT
#include "pb_heater.h"
#include "pb_heater_pid.h"
#include "pb_board.h"
#include "pb_ntc.h"

#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "pb_heater";

// Serializes fault-latch NVS writes (do_latch persist, clear_fault, tick retry)
// so a concurrent clear and a deferred-persist retry can't reorder into a stale
// write that resurrects a just-cleared latch. NULL until pb_heater_init().
static SemaphoreHandle_t s_persist_lock;

// Heater state is touched by BOTH the control task (pb_heater_tick, via the
// 2 Hz loop) and the HTTP task (set_target / notify_link_alive / clear_fault /
// getters). All of it is serialized under this spinlock — in particular the
// 64-bit s_last_link_us would tear on the 32-bit RISC-V core otherwise.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static float       s_target_c;      // guarded by s_mux
static float       s_control_chamber_c; // guarded by s_mux; optional external regulation temp
static bool        s_latched_off;   // guarded by s_mux (set by a safety trip)
static bool        s_inhibited;     // guarded by s_mux (PERMANENT; reboot-only)
static int64_t     s_last_link_us;  // guarded by s_mux
static const char *s_fault_reason;  // guarded by s_mux (points at a string literal)
static pb_fault_reason_t s_fault_code; // guarded by s_mux (machine-readable, persisted)
static bool        s_persist_pending;  // guarded by s_mux — a latch persist not yet committed
                                       // to NVS; retried from pb_heater_tick until it lands
static int64_t     s_persist_retry_us; // guarded by s_mux — last persist-retry timestamp
static pb_fault_reason_t s_persist_code; // guarded by s_mux — the cause of the current latch
                                         // that NVS holds or must hold (written, or pending a
                                         // retry); PB_FAULT_NONE = this latch is not for NVS
static bool        s_on;            // written only by the control task; atomic read
static pb_heater_pid_state_t s_pid; // control-task only — shared dc_pid state + SSR window
static float       s_commanded_duty; // guarded by s_mux; PID output after approach limit
static float       s_approach_limit; // guarded by s_mux; product-owned active duty ceiling
static pb_heater_constraint_t s_constraint; // guarded by s_mux
static bool        s_fb_cut;        // control-task only — element-foldback hysteresis latch:
                                    // true while the SSR is force-cut for element over-temp
                                    // (holds until the element cools below the resume point)
static bool        s_local_cut;     // control-task only — local chamber soft-limit latch used
                                    // only while a remote chamber temperature controls regulation
static float       s_max_target_c;      // guarded by s_mux — settable set-point ceiling
static int64_t     s_comms_timeout_us;  // guarded by s_mux — comms deadman (microseconds)
static float       s_cool_release_c;    // guarded by s_mux — residual-heat purge "cool down to" temp
static float       s_fb_cut_c;          // guarded by s_mux — user foldback-cut override (0 = auto/per-Rref)
static uint8_t     s_method;            // guarded by s_mux — pb_heater_method_t (control method)
static bool        s_heat_intent;       // control-task only — bang-bang chamber hysteresis latch
                                        // (PB_HEATER_HYSTERESIS_C band, defined in pb_heater.h)

// Persisted settings live in the shared app_nvs namespace (centi-°C / ms u32).
#define NVS_NS             "app_nvs"
#define KEY_HEAT_MAX_C     "heat_max_c"      // u32 centi-°C
#define KEY_HEAT_COMMS_MS  "heat_comms_ms"   // u32 ms
#define KEY_COOL_REL_C     "cool_rel_c"      // u32 centi-°C — cooldown-purge release temp
#define KEY_FB_CUT_C       "fb_cut_c"        // u32 centi-°C — foldback-cut override (0 = auto)
#define KEY_HEAT_METHOD    "heat_method"     // u8 pb_heater_method_t (0=bangbang default, 1=pid)
#define KEY_FAULT_CODE     "fault_code"      // u8 pb_fault_reason_t — THE persisted latch (NONE = not latched)
#define KEY_FAULT_LATCH    "fault_latch"     // u8 0/1 — legacy (older firmware); can only add a latch, a clear erases it

// Canonical, stable strings for each fault code (index == code). Kept identical to
// the historical trip strings so the API contract doesn't change for existing
// causes. Any out-of-range/corrupt code maps to a generic latched-fault string.
static const char *const k_fault_str[PB_FAULT__COUNT] = {
    [PB_FAULT_NONE]             = "none",
    [PB_FAULT_PTC_OVERTEMP]     = "PTC element over-temp",
    [PB_FAULT_CHAMBER_OVERTEMP] = "chamber over-temp",
    [PB_FAULT_CHAMBER_SENSOR]   = "chamber sensor fault",
    [PB_FAULT_PTC_SENSOR]       = "PTC sensor fault",
    [PB_FAULT_LINK_LOST]        = "controller link lost",
    [PB_FAULT_PANIC_OFF]        = "panic-off",
    [PB_FAULT_INHIBITED]        = "inhibited",
    [PB_FAULT_EMERGENCY]        = "safety trip",
    [PB_FAULT_NVS_UNREADABLE]   = "persisted fault state unreadable",
};

const char *pb_heater_fault_str(pb_fault_reason_t code)
{
    if ((unsigned)code >= PB_FAULT__COUNT || !k_fault_str[code]) return "latched fault";
    return k_fault_str[code];
}
// pb_heater_fault_restore() is a pure header inline (see pb_heater.h) so the
// boot-time fail-safe logic can be host-tested without an NVS backend.

static float centi_to_c(uint32_t v) { return (float)v / 100.0f; }
static uint32_t c_to_centi(float c)
{
    if (c < 0.0f)   c = 0.0f;
    if (c > 200.0f) c = 200.0f;
    return (uint32_t)(c * 100.0f + 0.5f);
}

static void ssr_set(bool on)        // control-task context only
{
#ifndef CONFIG_PB_DEVBOARD_SAFE
    gpio_set_level(PB_GPIO_RELAY, on ? 1 : 0);
#endif
    s_on = on;
}

static void telemetry_set(float commanded_duty, float approach_limit,
                          pb_heater_constraint_t constraint)
{
    taskENTER_CRITICAL(&s_mux);
    s_commanded_duty = commanded_duty;
    s_approach_limit = approach_limit;
    s_constraint = constraint;
    taskEXIT_CRITICAL(&s_mux);
}

void pb_heater_get_telemetry(pb_heater_telemetry_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_mux);
    out->commanded_duty = s_commanded_duty;
    out->approach_limit = s_approach_limit;
    out->constraint = s_constraint;
    taskEXIT_CRITICAL(&s_mux);
}

const char *pb_heater_constraint_str(pb_heater_constraint_t constraint)
{
    switch (constraint) {
    case PB_HEATER_CONSTRAINT_OFF:             return "off";
    case PB_HEATER_CONSTRAINT_NONE:            return "none";
    case PB_HEATER_CONSTRAINT_APPROACH_LIMIT:  return "approach_limit";
    case PB_HEATER_CONSTRAINT_TARGET_REACHED:  return "target_reached";
    case PB_HEATER_CONSTRAINT_LOCAL_FOLDBACK:  return "local_foldback";
    case PB_HEATER_CONSTRAINT_ELEMENT_FOLDBACK:return "element_foldback";
    case PB_HEATER_CONSTRAINT_PID_ERROR:       return "pid_error";
    default:                                   return "unknown";
    }
}

esp_err_t pb_heater_init(void)
{
    if (!s_persist_lock) s_persist_lock = xSemaphoreCreateMutex();
#ifndef CONFIG_PB_DEVBOARD_SAFE
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << PB_GPIO_RELAY),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;
#endif
    ssr_set(false);                  // guaranteed OFF before any request
    pb_heater_pid_reset(&s_pid);
    s_heat_intent = false;
    taskENTER_CRITICAL(&s_mux);
    s_method = PB_HEATER_METHOD_BANGBANG;   // safe default until load_config runs
    s_target_c = 0.0f;
    s_control_chamber_c = NAN;
    s_latched_off = false;
    s_persist_code = PB_FAULT_NONE;
    s_fault_reason = NULL;
    s_fault_code = PB_FAULT_NONE;
    s_last_link_us = esp_timer_get_time();
    s_commanded_duty = 0.0f;
    s_approach_limit = 0.0f;
    s_constraint = PB_HEATER_CONSTRAINT_OFF;
    // Conservative defaults ONLY — nvs isn't up yet; pb_heater_load_config()
    // applies persisted values later (called after nvs_init in app_main).
    s_max_target_c = PB_HEATER_MAX_TARGET_C_DEFAULT;
    s_comms_timeout_us = (int64_t)PB_HEATER_COMMS_TIMEOUT_MS_DEFAULT * 1000;
    s_cool_release_c = PB_HEATER_COOL_RELEASE_C_DEFAULT;
    s_fb_cut_c = 0.0f;   // auto (per-Rref default) until a user override is loaded/set
    taskEXIT_CRITICAL(&s_mux);
#ifdef CONFIG_PB_DEVBOARD_SAFE
    ESP_LOGW(TAG, "safe dev-board backend: relay GPIO compiled out");
#else
    ESP_LOGI(TAG, "init: SSR forced OFF");
#endif
    return ESP_OK;
}

esp_err_t pb_heater_set_target_c(float target_c)
{
    if (!isfinite(target_c)) {           // reject NaN / +-Inf defensively
        ESP_LOGW(TAG, "rejected non-finite target");
        return ESP_ERR_INVALID_ARG;
    }
    if (target_c < 0.0f) target_c = 0.0f;

    esp_err_t r = ESP_OK;
    taskENTER_CRITICAL(&s_mux);
    if (target_c > s_max_target_c) target_c = s_max_target_c;   // clamp to the live ceiling
    if ((s_latched_off || s_inhibited) && target_c > 0.0f) {
        r = ESP_ERR_INVALID_STATE;       // never queue heat behind a fault/inhibit
    } else {
        s_target_c = target_c;
    }
    taskEXIT_CRITICAL(&s_mux);

    if (r == ESP_OK)
        ESP_LOGI(TAG, "target set to %.1f C", target_c);
    else
        ESP_LOGW(TAG, "target %.1f rejected: fault latched (clear fault, then issue a fresh command)", target_c);
    return r;
}

float pb_heater_get_target_c(void)
{
    taskENTER_CRITICAL(&s_mux);
    float t = s_target_c;
    taskEXIT_CRITICAL(&s_mux);
    return t;
}

void pb_heater_set_control_chamber_c(float temp_c)
{
    taskENTER_CRITICAL(&s_mux);
    s_control_chamber_c = isfinite(temp_c) ? temp_c : NAN;
    taskEXIT_CRITICAL(&s_mux);
}

void pb_heater_load_config(void)
{
    // Read NVS OUTSIDE the lock (it can block), then clamp + assign under s_mux.
    float    max_c    = PB_HEATER_MAX_TARGET_C_DEFAULT;
    uint32_t comms_ms = PB_HEATER_COMMS_TIMEOUT_MS_DEFAULT;
    float    cool_c   = PB_HEATER_COOL_RELEASE_C_DEFAULT;
    float    fb_cut   = 0.0f;   // 0 = auto (per-Rref default)
    uint8_t  method   = PB_HEATER_METHOD_BANGBANG;   // default: v1.1.15 bang-bang
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t v;
        uint8_t  b;
        if (nvs_get_u32(h, KEY_HEAT_MAX_C, &v) == ESP_OK)    max_c    = centi_to_c(v);
        if (nvs_get_u32(h, KEY_HEAT_COMMS_MS, &v) == ESP_OK) comms_ms = v;
        if (nvs_get_u32(h, KEY_COOL_REL_C, &v) == ESP_OK)    cool_c   = centi_to_c(v);
        if (nvs_get_u32(h, KEY_FB_CUT_C, &v) == ESP_OK)      fb_cut   = centi_to_c(v);
        if (nvs_get_u8(h, KEY_HEAT_METHOD, &b) == ESP_OK)    method   = b;
        nvs_close(h);
    }
    // Any unknown/corrupt method value falls back to the safe bang-bang default.
    if (method != PB_HEATER_METHOD_PID) method = PB_HEATER_METHOD_BANGBANG;
    // Clamp to the safe envelope regardless of what NVS held (defends against a
    // corrupted/hand-edited store — the ceiling can never exceed the ABS max).
    if (max_c < PB_HEATER_MIN_TARGET_C)     max_c = PB_HEATER_MIN_TARGET_C;
    if (max_c > PB_HEATER_ABS_MAX_TARGET_C) max_c = PB_HEATER_ABS_MAX_TARGET_C;
    if (comms_ms < PB_HEATER_COMMS_TIMEOUT_MS_MIN) comms_ms = PB_HEATER_COMMS_TIMEOUT_MS_MIN;
    if (comms_ms > PB_HEATER_COMMS_TIMEOUT_MS_MAX) comms_ms = PB_HEATER_COMMS_TIMEOUT_MS_MAX;
    if (cool_c < PB_HEATER_COOL_RELEASE_MIN_C) cool_c = PB_HEATER_COOL_RELEASE_MIN_C;
    if (cool_c > PB_HEATER_COOL_RELEASE_MAX_C) cool_c = PB_HEATER_COOL_RELEASE_MAX_C;
    // fb_cut: 0 stays "auto"; any positive value is clamped to the safe soft-foldback band.
    if (fb_cut > 0.0f) {
        if (fb_cut < PB_HEATER_FB_CUT_MIN_C) fb_cut = PB_HEATER_FB_CUT_MIN_C;
        if (fb_cut > PB_HEATER_FB_CUT_MAX_C) fb_cut = PB_HEATER_FB_CUT_MAX_C;
    }
    taskENTER_CRITICAL(&s_mux);
    s_max_target_c = max_c;
    s_comms_timeout_us = (int64_t)comms_ms * 1000;
    s_cool_release_c = cool_c;
    s_fb_cut_c = fb_cut;
    s_method = method;
    if (s_target_c > s_max_target_c) s_target_c = s_max_target_c;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "config: max_target=%.1fC comms_timeout=%ums cool_release=%.1fC fb_cut=%.1fC method=%s",
             max_c, (unsigned)comms_ms, cool_c, fb_cut,
             method == PB_HEATER_METHOD_PID ? "pid" : "bangbang");
}

esp_err_t pb_heater_set_max_target_c(float max_c)
{
    if (!isfinite(max_c)) return ESP_ERR_INVALID_ARG;
    if (max_c < PB_HEATER_MIN_TARGET_C)     max_c = PB_HEATER_MIN_TARGET_C;
    if (max_c > PB_HEATER_ABS_MAX_TARGET_C) max_c = PB_HEATER_ABS_MAX_TARGET_C;   // never past 70
    taskENTER_CRITICAL(&s_mux);
    s_max_target_c = max_c;
    if (s_target_c > s_max_target_c) s_target_c = s_max_target_c;   // pull a live target down
    taskEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;                                                  // persist outside the lock
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, KEY_HEAT_MAX_C, c_to_centi(max_c));
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "max_target set to %.1f C", max_c);
    return ESP_OK;
}

float pb_heater_get_max_target_c(void)
{
    taskENTER_CRITICAL(&s_mux);
    float m = s_max_target_c;
    taskEXIT_CRITICAL(&s_mux);
    return m;
}

esp_err_t pb_heater_set_comms_timeout_ms(uint32_t ms)
{
    if (ms < PB_HEATER_COMMS_TIMEOUT_MS_MIN) ms = PB_HEATER_COMMS_TIMEOUT_MS_MIN;
    if (ms > PB_HEATER_COMMS_TIMEOUT_MS_MAX) ms = PB_HEATER_COMMS_TIMEOUT_MS_MAX;  // never > 5 min
    taskENTER_CRITICAL(&s_mux);
    s_comms_timeout_us = (int64_t)ms * 1000;
    taskEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, KEY_HEAT_COMMS_MS, ms);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "comms_timeout set to %u ms", (unsigned)ms);
    return ESP_OK;
}

uint32_t pb_heater_get_comms_timeout_ms(void)
{
    taskENTER_CRITICAL(&s_mux);
    int64_t us = s_comms_timeout_us;
    taskEXIT_CRITICAL(&s_mux);
    return (uint32_t)(us / 1000);
}

esp_err_t pb_heater_set_cool_release_c(float c)
{
    if (!isfinite(c)) return ESP_ERR_INVALID_ARG;
    if (c < PB_HEATER_COOL_RELEASE_MIN_C) c = PB_HEATER_COOL_RELEASE_MIN_C;
    if (c > PB_HEATER_COOL_RELEASE_MAX_C) c = PB_HEATER_COOL_RELEASE_MAX_C;
    taskENTER_CRITICAL(&s_mux);
    s_cool_release_c = c;
    taskEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;                                 // persist outside the lock
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, KEY_COOL_REL_C, c_to_centi(c));
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "cool_release set to %.1f C", c);
    return ESP_OK;
}

float pb_heater_get_cool_release_c(void)
{
    taskENTER_CRITICAL(&s_mux);
    float c = s_cool_release_c;
    taskEXIT_CRITICAL(&s_mux);
    return c;
}

esp_err_t pb_heater_set_fb_cut_c(float c)
{
    if (!isfinite(c)) return ESP_ERR_INVALID_ARG;
    if (c <= 0.0f) {
        c = 0.0f;                                   // clear override -> auto (per-Rref)
    } else {
        if (c < PB_HEATER_FB_CUT_MIN_C) c = PB_HEATER_FB_CUT_MIN_C;
        if (c > PB_HEATER_FB_CUT_MAX_C) c = PB_HEATER_FB_CUT_MAX_C;
    }
    taskENTER_CRITICAL(&s_mux);
    s_fb_cut_c = c;
    taskEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;                                 // persist outside the lock
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, KEY_FB_CUT_C, c_to_centi(c));   // 0 = auto
        nvs_commit(h);
        nvs_close(h);
    }
    if (c > 0.0f) ESP_LOGI(TAG, "foldback cut override set to %.1f C", c);
    else          ESP_LOGI(TAG, "foldback cut override cleared (auto/per-Rref)");
    return ESP_OK;
}

float pb_heater_get_fb_cut_c(void)
{
    taskENTER_CRITICAL(&s_mux);
    float c = s_fb_cut_c;
    taskEXIT_CRITICAL(&s_mux);
    return c;
}

esp_err_t pb_heater_set_method(pb_heater_method_t method)
{
    if (method != PB_HEATER_METHOD_BANGBANG && method != PB_HEATER_METHOD_PID)
        return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_mux);
    s_method = (uint8_t)method;
    taskEXIT_CRITICAL(&s_mux);
    nvs_handle_t h;                                 // persist outside the lock
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, KEY_HEAT_METHOD, (uint8_t)method);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "control method set to %s",
             method == PB_HEATER_METHOD_PID ? "pid" : "bangbang");
    return ESP_OK;
}

pb_heater_method_t pb_heater_get_method(void)
{
    taskENTER_CRITICAL(&s_mux);
    uint8_t m = s_method;
    taskEXIT_CRITICAL(&s_mux);
    return (pb_heater_method_t)m;
}

void pb_heater_notify_link_alive(void)
{
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    s_last_link_us = now;
    taskEXIT_CRITICAL(&s_mux);
}

// Persist the safety-fault latch to NVS: a cause latches it (best-effort — RAM
// is authoritative for the session). NVS ops MUST run outside s_mux (they can
// block / need interrupts).
//
// The latch is ONE key, so one write changes it. NVS makes one key-value pair
// atomic under power loss, not two (ESP-IDF docs/en/api-reference/storage/
// nvs_flash.rst, "Security, Tampering, and Robustness"): a power loss leaves the
// old value or the new value.
// Write one fault key. ESP_ERR_NVS_REMOVE_FAILED means that the new value is
// in flash and only the erase of the old entry (a flash write error) failed:
// https://github.com/espressif/esp-idf/blob/v5.3.6/components/nvs_flash/include/nvs.h#L37
// https://github.com/espressif/esp-idf/blob/v5.3.6/components/nvs_flash/src/nvs_storage.cpp#L595-L599
// nvs_flash_init removes the old duplicate only if it is on the same page as
// the new entry, or if the new entry is the last item of the last page:
// https://github.com/espressif/esp-idf/blob/v5.3.6/components/nvs_flash/src/nvs_page.cpp#L726-L727
// https://github.com/espressif/esp-idf/blob/v5.3.6/components/nvs_flash/src/nvs_pagemanager.cpp#L57-L89
// Otherwise the lookup starts at the oldest page, and the old value can win:
// https://github.com/espressif/esp-idf/blob/v5.3.6/components/nvs_flash/src/nvs_storage.cpp#L256-L270
// The firmware cannot fix that case: a retry finds the new value and writes
// nothing. Thus count the write as done. If this returned the error, a clear
// that reached flash would report a failure, and RAM would stay latched.
static esp_err_t set_fault_key(nvs_handle_t h, const char *key, uint8_t v)
{
    esp_err_t err = nvs_set_u8(h, key, v);
    return err == ESP_ERR_NVS_REMOVE_FAILED ? ESP_OK : err;
}

static esp_err_t persist_fault(pb_fault_reason_t code)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "fault persist: nvs_open %s", esp_err_to_name(err)); return err; }
    err = set_fault_key(h, KEY_FAULT_CODE, (uint8_t)code);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "fault persist: %s", esp_err_to_name(err));
    return err;
}

// Clear the latch in NVS. The return matters: a failed clear is surfaced (HTTP
// 500) rather than falsely reporting the fault gone, and flash must stay latched.
// Without the legacy key (firmware before the one-key format), this is one
// write. If flash holds no latch (legacy 0 and no held cause), erase the legacy
// key, then write NONE. Otherwise (or if the legacy read fails), every step must
// leave flash latched: write a cause into fault_code (`held`, or a generic one),
// erase fault_latch, then write NONE.
static esp_err_t persist_clear(pb_fault_reason_t held)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "fault clear: nvs_open %s", esp_err_to_name(err)); return err; }
    uint8_t legacy;
    err = nvs_get_u8(h, KEY_FAULT_LATCH, &legacy);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err == ESP_OK && legacy == 0 && held == PB_FAULT_NONE) {
        // Flash holds no latch (a RAM-only latch is cleared). Each step below
        // leaves flash unlatched, so do not write a cause.
        err = nvs_erase_key(h, KEY_FAULT_LATCH);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        // The legacy key can hold a latch, or the read failed: take the safe path.
        err = set_fault_key(h, KEY_FAULT_CODE,
                            (uint8_t)(held != PB_FAULT_NONE ? held : PB_FAULT_EMERGENCY));
        if (err == ESP_OK) err = nvs_erase_key(h, KEY_FAULT_LATCH);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = set_fault_key(h, KEY_FAULT_CODE, PB_FAULT_NONE);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "fault clear: %s", esp_err_to_name(err));
    return err;
}

// The hazard-driven causes of SAFETY.md ("Persistent fault latch").
static bool is_hazard(pb_fault_reason_t code)
{
    return code == PB_FAULT_PTC_OVERTEMP || code == PB_FAULT_CHAMBER_OVERTEMP ||
           code == PB_FAULT_CHAMBER_SENSOR || code == PB_FAULT_PTC_SENSOR ||
           code == PB_FAULT_LINK_LOST;
}

typedef enum {
    LATCH_RAM_ONLY,      // panic-off, permanent inhibit, boot NVS_UNREADABLE: never written
    LATCH_PERSIST,       // hazard trips and emergency_off: written to NVS
    LATCH_FROM_NVS,      // boot restore: NVS already holds it
} latch_nvs_t;

// Latch the heater off with a machine-readable code + live reason string. Sets RAM
// under the lock, then, for LATCH_PERSIST, writes NVS if NVS does not hold this
// latch yet — also when a hazard trips on top of a RAM-only latch — or if a hazard
// cause replaces a persisted non-hazard cause (emergency_off). So an episode writes
// NVS at most twice, and repeated trips while hot do not write it. LATCH_RAM_ONLY
// leaves NVS untouched: panic-off and the permanent inhibit must clear on a power
// cycle. drive_ssr is CONTROL-TASK ONLY (it writes the GPIO).
static void do_latch(pb_fault_reason_t code, const char *reason,
                     bool inhibit, bool drive_ssr, latch_nvs_t nvs)
{
    if (drive_ssr) ssr_set(false);
    bool write;
    taskENTER_CRITICAL(&s_mux);
    write = nvs == LATCH_PERSIST &&
            (s_persist_code == PB_FAULT_NONE || (is_hazard(code) && !is_hazard(s_persist_code)));
    if (write || nvs == LATCH_FROM_NVS) s_persist_code = code;
    s_target_c = 0.0f;
    s_latched_off = true;
    if (inhibit) s_inhibited = true;
    s_fault_code = code;
    s_fault_reason = reason ? reason : pb_heater_fault_str(code);
    s_commanded_duty = 0.0f;
    s_approach_limit = 0.0f;
    s_constraint = PB_HEATER_CONSTRAINT_OFF;
    taskEXIT_CRITICAL(&s_mux);
    if (write) {
        if (s_persist_lock) xSemaphoreTake(s_persist_lock, portMAX_DELAY);
        esp_err_t pe = persist_fault(code);
        if (pe != ESP_OK) {
            // The write failed (NVS busy, or a fault before nvs_init on early boot).
            // Mark it pending so pb_heater_tick() keeps retrying until it commits —
            // otherwise a reboot would lose a latch that only ever lived in RAM.
            // Set it before the persist lock is released, so a clear that takes
            // the lock next also drops the pending retry.
            taskENTER_CRITICAL(&s_mux);
            s_persist_pending = true;
            taskEXIT_CRITICAL(&s_mux);
        }
        if (s_persist_lock) xSemaphoreGive(s_persist_lock);
        if (pe != ESP_OK) {
            ESP_LOGW(TAG, "fault persist deferred (will retry): %s", pb_heater_fault_str(code));
        }
    }
}

// Control-task safety trip: latch (driving the SSR down) + persist + log.
static void trip(pb_fault_reason_t code)
{
    do_latch(code, NULL, /*inhibit=*/false, /*drive_ssr=*/true, LATCH_PERSIST);
    ESP_LOGW(TAG, "EMERGENCY OFF: %s", pb_heater_fault_str(code));
}

void pb_heater_emergency_off(const char *reason)   // control-task context
{
    do_latch(PB_FAULT_EMERGENCY, reason, /*inhibit=*/false, /*drive_ssr=*/true, LATCH_PERSIST);
    ESP_LOGW(TAG, "EMERGENCY OFF: %s", reason ? reason : "(unspecified)");
}

void pb_heater_request_panic_off(const char *reason)   // any task
{
    // Latch only — NO GPIO write. Unlike pb_heater_emergency_off(), this is safe
    // to call off the control task (e.g. the button task): it preserves the
    // single-SSR-writer invariant by leaving the actual ssr_set(false) to the
    // next pb_heater_tick() on the control task. Callers that need the SSR down
    // fast should wake the control task immediately after this returns.
    // LATCH_RAM_ONLY: a user panic-off is a manual stop, documented to clear on
    // reboot (only hazard-driven trips — over-temp/sensor/comms — persist).
    do_latch(PB_FAULT_PANIC_OFF, reason, /*inhibit=*/false, /*drive_ssr=*/false, LATCH_RAM_ONLY);
}

esp_err_t pb_heater_clear_fault(void)
{
    bool was;
    taskENTER_CRITICAL(&s_mux);
    // A permanent inhibit is NOT clearable — leave everything latched.
    if (s_inhibited) {
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "clear ignored: heater permanently inhibited (reboot required)");
        return ESP_ERR_INVALID_STATE;
    }
    was = s_latched_off;
    taskEXIT_CRITICAL(&s_mux);

    // Persist-first, under the persist lock so a concurrent tick retry can't write
    // a stale "latched" after this clears NVS. If the persist fails, leave the fault
    // latched (fail-safe) and report it — the heater stays off rather than falsely
    // appearing cleared but returning on the next reboot.
    if (s_persist_lock) xSemaphoreTake(s_persist_lock, portMAX_DELAY);
    taskENTER_CRITICAL(&s_mux);
    pb_fault_reason_t held = s_persist_code;
    taskEXIT_CRITICAL(&s_mux);
    esp_err_t err = persist_clear(held);
    if (err != ESP_OK) {
        // A step before the error can have changed flash (persist_clear writes
        // a cause first). Let the tick retry write the held cause again.
        taskENTER_CRITICAL(&s_mux);
        if (s_persist_code != PB_FAULT_NONE) s_persist_pending = true;
        taskEXIT_CRITICAL(&s_mux);
        if (s_persist_lock) xSemaphoreGive(s_persist_lock);
        ESP_LOGE(TAG, "clear rejected: NVS clear failed (%s) — fault stays latched", esp_err_to_name(err));
        return err;
    }
    taskENTER_CRITICAL(&s_mux);
    s_latched_off = false;
    s_persist_code = PB_FAULT_NONE;
    s_target_c = 0.0f;              // reset leaves the target at ZERO — a fresh
    s_fault_reason = NULL;          // target command is required to resume heating.
    s_fault_code = PB_FAULT_NONE;
    s_persist_pending = false;      // NVS now holds "not latched"; nothing to retry
    taskEXIT_CRITICAL(&s_mux);
    if (s_persist_lock) xSemaphoreGive(s_persist_lock);
    if (was) ESP_LOGW(TAG, "fault latch cleared; target reset to 0 (send a fresh target to resume)");
    return ESP_OK;
}

void pb_heater_inhibit(const char *reason)
{
    // Permanent (reboot-only) -> LATCH_RAM_ONLY: a power cycle must lift it, so
    // persisting could brick the device across reboots on a transient init failure.
    // Any clearable latch already in NVS is left intact.
    do_latch(PB_FAULT_INHIBITED, reason, /*inhibit=*/true, /*drive_ssr=*/true, LATCH_RAM_ONLY);
    ESP_LOGE(TAG, "HEATER INHIBITED (reboot-only): %s", reason ? reason : "(unspecified)");
}

bool pb_heater_is_inhibited(void) { return s_inhibited; }   // atomic bool read
bool pb_heater_is_faulted(void) { return s_latched_off || s_inhibited; }
bool pb_heater_is_on(void) { return s_on; }                 // atomic bool read

const char *pb_heater_fault_reason(void)
{
    taskENTER_CRITICAL(&s_mux);
    const char *r = s_fault_reason;
    taskEXIT_CRITICAL(&s_mux);
    return r;
}

pb_fault_reason_t pb_heater_fault_code(void)
{
    taskENTER_CRITICAL(&s_mux);
    pb_fault_reason_t c = s_fault_code;
    taskEXIT_CRITICAL(&s_mux);
    return c;
}

static pb_nvs_read_t nvs_read_result(esp_err_t err)
{
    if (err == ESP_OK) return PB_NVS_OK;
    return err == ESP_ERR_NVS_NOT_FOUND ? PB_NVS_NOT_FOUND : PB_NVS_ERROR;
}

void pb_heater_load_fault(void)
{
    nvs_handle_t h;
    esp_err_t oe = nvs_open(NVS_NS, NVS_READONLY, &h);
    uint8_t latch_val = 0, code_val = PB_FAULT_NONE;
    pb_nvs_read_t latch_rd = PB_NVS_NOT_FOUND, code_rd = PB_NVS_NOT_FOUND;
    if (oe == ESP_OK) {
        latch_rd = nvs_read_result(nvs_get_u8(h, KEY_FAULT_LATCH, &latch_val));
        code_rd  = nvs_read_result(nvs_get_u8(h, KEY_FAULT_CODE, &code_val));
        nvs_close(h);
    }
    pb_fault_reason_t code;
    bool latched = pb_heater_fault_restore(nvs_read_result(oe), latch_rd, latch_val,
                                           code_rd, code_val, &code);
    if (latched) {
        // No NVS write (already in NVS, or unreadable); drive_ssr=false (init()
        // already forced the SSR off and the control task is not running yet).
        // An unreadable store does not hold the latch: a later hazard trip must
        // still write it.
        do_latch(code, NULL, /*inhibit=*/false, /*drive_ssr=*/false,
                 code == PB_FAULT_NVS_UNREADABLE ? LATCH_RAM_ONLY : LATCH_FROM_NVS);
        ESP_LOGW(TAG, "boot: restored latched fault: %s", pb_heater_fault_str(code));
    }
}

bool pb_heater_heat_mode(void)     // "armed and not tripped" (independent of SSR cycling)
{
    taskENTER_CRITICAL(&s_mux);
    bool m = (s_target_c > 0.0f && !s_latched_off);
    taskEXIT_CRITICAL(&s_mux);
    return m;
}

void pb_heater_tick(void)          // control-task context; sole writer of s_on
{
    // Retry a deferred fault-latch persist until it commits, so a latch that
    // couldn't be written when it happened (NVS busy, or a fault before nvs_init)
    // still survives a reboot. Throttled and serialized with clear_fault via the
    // persist lock; re-reads the latch state INSIDE the lock so a clear that landed
    // meanwhile is never overwritten with a stale "latched".
    bool pending;
    taskENTER_CRITICAL(&s_mux);
    pending = s_persist_pending;
    taskEXIT_CRITICAL(&s_mux);
    if (pending) {
        int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&s_mux);
        bool due = (now - s_persist_retry_us) >= 2000000;   // ~2 s: bound NVS churn/log spam
        taskEXIT_CRITICAL(&s_mux);
        if (due && s_persist_lock && xSemaphoreTake(s_persist_lock, 0) == pdTRUE) {
            taskENTER_CRITICAL(&s_mux);
            pb_fault_reason_t code = s_persist_code;   // the hazard, not a later panic-off
            bool still = code != PB_FAULT_NONE;         // a clear sets NONE: nothing to write
            s_persist_retry_us = now;
            taskEXIT_CRITICAL(&s_mux);
            esp_err_t pr = still ? persist_fault(code) : ESP_OK;  // cleared -> nothing to write
            if (pr == ESP_OK) {
                taskENTER_CRITICAL(&s_mux);
                s_persist_pending = false;
                taskEXIT_CRITICAL(&s_mux);
                ESP_LOGI(TAG, "deferred fault persist committed");
            }
            xSemaphoreGive(s_persist_lock);
        }
    }

    float   target;
    float   control_chamber_c;
    bool    latched;
    int64_t last_link;
    int64_t comms_timeout_us;
    taskENTER_CRITICAL(&s_mux);
    target = s_target_c;
    control_chamber_c = s_control_chamber_c;
    latched = s_latched_off;
    last_link = s_last_link_us;
    comms_timeout_us = s_comms_timeout_us;
    taskEXIT_CRITICAL(&s_mux);

    const bool armed = (target > 0.0f);
    // LED indication is owned by pb_leds (driven from pb_policy) — pb_heater no
    // longer touches any LED.

    float ptc_c = 0.0f, chamber_c = 0.0f;
    pb_ntc_status_t ps = pb_ntc_read(PB_NTC_PTC, &ptc_c);
    pb_ntc_status_t cs = pb_ntc_read(PB_NTC_CHAMBER, &chamber_c);

    // All safety trips (over-temp cutoffs, fail-closed sensor faults, comms-loss
    // watchdog) are decided by the pure, host-tested pb_heater_eval_trip() so the
    // load-bearing priority ordering has one authoritative definition. Over-temp is
    // trusted only on a valid sensor; while armed, EITHER thermistor being anything
    // other than OK — OPEN, SHORT, or a UNINIT read error — is fail-closed (a blind
    // heater or unmonitored element must latch off), as is a comms deadman timeout.
    pb_fault_reason_t trip_code = pb_heater_eval_trip(
        ps == PB_NTC_OK, ptc_c, cs == PB_NTC_OK, chamber_c, armed,
        (esp_timer_get_time() - last_link) > comms_timeout_us);
    if (trip_code != PB_FAULT_NONE) {
        trip(trip_code);
        return;
    }

    if (latched || !armed) {
        if (s_on) ssr_set(false);
        pb_heater_pid_reset(&s_pid);
        s_heat_intent = false;   // drop the bang-bang + foldback latches so the next
        s_fb_cut      = false;   // arm starts clean rather than mid-cycle
        s_local_cut   = false;
        telemetry_set(0.0f, 0.0f, PB_HEATER_CONSTRAINT_OFF);
        return;
    }

    // Here: armed && !latched && cs==OK && ps==OK.
    // Step 1 — select the process variable. Policy supplies a finite external value
    // only for AUTO mode with fresh Bambu telemetry and the persisted preference on;
    // NAN means use DragonBreath's local chamber NTC. Both sources feed this same PID
    // path. Local sensors above remain authoritative for every safety decision.
    bool external_regulation;
    float regulation_c = pb_heater_pid_process_variable(
        chamber_c, control_chamber_c, &external_regulation);
    pb_heater_pid_set_source(&s_pid, external_regulation);

    // Step 2 — local chamber soft foldback for REMOTE regulation. The printer sensor
    // represents bulk chamber air, while DragonBreath's local chamber NTC can see much
    // hotter outlet/near-heater air. When an external control temperature is active, cut
    // heat at the local soft ceiling and hold it off until the local region cools through
    // the resume threshold. The fixed 85 C chamber trip above remains the latching backstop.
    if (external_regulation)
        s_local_cut = pb_heater_local_foldback_cut(cs == PB_NTC_OK, chamber_c, s_local_cut);
    else
        s_local_cut = false;

    // Step 3 — element-temperature foldback (hysteresis hard-cut). While the PTC element
    // is too hot we force the SSR fully OFF and hold it off until the element cools below
    // the resume point — a real cool-down cycle rather than half-power hovering near the
    // 105 C hard cutoff. Thresholds are per board Rref variant (33k boards run hotter, so
    // they get a lower cut). Pure + host-tested; ps==OK is guaranteed here (checked above).
    float fb_cut, fb_resume;
    pb_heater_effective_foldback(pb_heater_get_fb_cut_c(), pb_ntc_rref_kohm(), &fb_cut, &fb_resume);
    s_fb_cut = pb_heater_foldback_cut(ps == PB_NTC_OK, ptc_c, s_fb_cut, fb_cut, fb_resume);

    // Either soft safety layer inhibits heat regardless of the requested duty; it
    // applies to both control methods below.
    bool safety_inhibited = s_local_cut || s_fb_cut;

    // Step 4 — decide duty by the selected control method (persisted; default
    // bang-bang v1.1.15). On a method change, reset BOTH paths so the newly
    // selected one starts from clean history.
    bool use_pid = (pb_heater_get_method() == PB_HEATER_METHOD_PID);
    static int s_tick_method_prev = -1;   // control-task only; -1 forces a first-tick sync
    if (s_tick_method_prev != (int)use_pid) {
        pb_heater_pid_reset(&s_pid);
        s_heat_intent = false;
        s_tick_method_prev = (int)use_pid;
    }

    if (!use_pid) {
        // Bang-bang (v1.1.15): chamber hysteresis decides the base demand; the local
        // and element foldback layers above still gate it, and the SSR is driven
        // full on/off with no time-proportioning.
        if (regulation_c < (target - PB_HEATER_HYSTERESIS_C))
            s_heat_intent = true;
        else if (regulation_c >= target)
            s_heat_intent = false;
        bool bb_drive = s_heat_intent && !safety_inhibited;
        pb_heater_constraint_t bb_constraint = PB_HEATER_CONSTRAINT_NONE;
        if (s_fb_cut)                    bb_constraint = PB_HEATER_CONSTRAINT_ELEMENT_FOLDBACK;
        else if (s_local_cut)            bb_constraint = PB_HEATER_CONSTRAINT_LOCAL_FOLDBACK;
        else if (regulation_c >= target) bb_constraint = PB_HEATER_CONSTRAINT_TARGET_REACHED;
        telemetry_set(bb_drive ? 1.0f : 0.0f, 1.0f, bb_constraint);
        if (bb_drive != s_on) ssr_set(bb_drive);
        return;
    }

    // PID / PTC (v1.1.16, opt-in). Advance the common chamber PID. If either local
    // soft safety layer inhibits the requested output, hold integration while still
    // updating the measurement/derivative history. The safety layers remain
    // downstream and authoritative regardless of PID demand.
    float duty = 0.0f;
    bool pid_ok = pb_heater_pid_step(&s_pid, target, regulation_c,
                                     !safety_inhibited, &duty);
    if (!pid_ok) {
        ESP_LOGE(TAG, "PID step rejected; forcing SSR off");
        duty = 0.0f;
    }

    // The soft approach cap actually applied this step (rate-gated inside the PID
    // step); 1.0 means unshaped. Reported so diagnostics can distinguish anti-
    // overshoot damping from element/local foldback.
    float approach_limit = s_pid.last_approach_cap;
    pb_heater_constraint_t constraint = PB_HEATER_CONSTRAINT_NONE;
    if (!pid_ok)
        constraint = PB_HEATER_CONSTRAINT_PID_ERROR;
    else if (s_fb_cut)
        constraint = PB_HEATER_CONSTRAINT_ELEMENT_FOLDBACK;
    else if (s_local_cut)
        constraint = PB_HEATER_CONSTRAINT_LOCAL_FOLDBACK;
    else if (regulation_c >= target)
        constraint = PB_HEATER_CONSTRAINT_TARGET_REACHED;
    else if (approach_limit < 1.0f && duty >= approach_limit - 0.0005f)
        constraint = PB_HEATER_CONSTRAINT_APPROACH_LIMIT;

    // This is the duty actually admitted to the SSR window after downstream
    // thermal governors. Keep the underlying PID state private; diagnostics
    // should report the command the actuator was allowed to receive.
    float commanded_duty = safety_inhibited ? 0.0f : duty;
    telemetry_set(commanded_duty, approach_limit, constraint);

    // Step 5 — time-proportion normalized duty through a slow 10 s window. This is
    // still plain on/off drive of a zero-cross SSR; there is no phase cutting.
    bool drive = pb_heater_pid_window_on(
        &s_pid, commanded_duty, esp_timer_get_time());
    if (drive != s_on) ssr_set(drive);
}
