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

// First-heat check (see pb_heater.h). The window state is control-task only;
// pb_heater_load_first_heat() also writes it, before the control task starts.
static pb_fh_state_t s_fh;
static int64_t     s_fh_t0_us, s_fh_last_us, s_fh_on_us;
static int32_t     s_fh_ptc0, s_fh_ch0;            // start readings, tenths
static int32_t     s_fh_ptc_first[2], s_fh_ch_first[2];   // first two readings of the window
static uint8_t     s_fh_nstart;                    // readings taken for the start (0..3)
static int32_t     s_fh_ptc_prev, s_fh_ch_prev;    // last readings, for the glitch filter
static int32_t     s_fh_ptc_peak, s_fh_ch_peak;
static int32_t     s_fh_ptc_rise, s_fh_ch_rise;
static uint32_t    s_fh_cfg;                       // mapping fingerprint at the start
static int64_t     s_fh_retry_us;
static uint32_t    s_fh_restarts, s_fh_skipped;    // published counters (this boot)
static uint32_t    s_fh_uncertain_streak;          // consecutive UNCERTAIN results (this boot)
static bool        s_fh_store_warned;              // under the persist lock: log one failure only
// Guarded by s_mux: the published snapshot, the re-run request and the NVS write
// that is requested or pending (the tick retries it).
static pb_heater_first_heat_t s_fh_pub;
static bool        s_fh_rerun_req;
static bool        s_fh_store_pending;
static uint32_t    s_fh_store_seq;
static uint8_t     s_fh_store_val;
static int32_t     s_fh_store_ptc, s_fh_store_ch;
static uint32_t    s_fh_store_cfg;

#define FH_ENFORCED (PB_FH_ENFORCED != 0)   // see pb_heater.h: dev board or REPORT_ONLY

// Persisted settings live in the shared app_nvs namespace (centi-°C / ms u32).
#define NVS_NS             "app_nvs"
#define KEY_HEAT_MAX_C     "heat_max_c"      // u32 centi-°C
#define KEY_HEAT_COMMS_MS  "heat_comms_ms"   // u32 ms
#define KEY_COOL_REL_C     "cool_rel_c"      // u32 centi-°C — cooldown-purge release temp
#define KEY_FB_CUT_C       "fb_cut_c"        // u32 centi-°C — foldback-cut override (0 = auto)
#define KEY_HEAT_METHOD    "heat_method"     // u8 pb_heater_method_t (0=bangbang default, 1=pid)
#define KEY_FAULT_LATCH    "fault_latch"     // u8 0/1 — persisted safety-fault latch
#define KEY_FAULT_CODE     "fault_code"      // u8 pb_fault_reason_t
#define KEY_FH_STATE       "fh_state"        // u8 pb_fh_stored_t — first-heat result
#define KEY_FH_PTC         "fh_ptc_dc"       // i32 tenths — measured PTC rise
#define KEY_FH_CH          "fh_ch_dc"        // i32 tenths — measured chamber rise
#define KEY_FH_CFG         "fh_cfg"          // u32 — mapping fingerprint of the result
#define KEY_STOCK_NTC_BIT  "ntc_bit"         // stock firmware: 4-byte blob, 0/1/2 (hint only)

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
    [PB_FAULT_HEATER_NO_RISE]   = "heater no-rise (first-heat check)",
    [PB_FAULT_SENSOR_ROLES]  = "sensor roles wrong or PTC sensor not on element (first-heat check)",
};

const char *pb_heater_fault_str(pb_fault_reason_t code)
{
    if ((unsigned)code >= PB_FAULT__COUNT || !k_fault_str[code]) return "latched fault";
    return k_fault_str[code];
}
// pb_heater_fault_decide() is a pure header inline (see pb_heater.h) so the
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
    s_fh_pub = (pb_heater_first_heat_t){ .state = PB_FH_IDLE, .enforced = FH_ENFORCED };
    s_fh_rerun_req = false;
    s_fh_store_pending = false;
    taskEXIT_CRITICAL(&s_mux);
    s_fh = PB_FH_IDLE;   // pb_heater_load_first_heat() restores a stored pass
    s_fh_nstart = 0;
    s_fh_restarts = s_fh_skipped = s_fh_uncertain_streak = 0;
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

// Persist (or clear) the safety-fault latch to NVS. NVS ops MUST run outside
// s_mux (they can block / need interrupts). On latch this is best-effort — RAM is
// authoritative for the session; on CLEAR the return matters so a failed persist
// can be surfaced (HTTP 500) rather than falsely reporting the fault gone.
static esp_err_t persist_fault(bool latched, pb_fault_reason_t code)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "fault persist: nvs_open %s", esp_err_to_name(err)); return err; }
    // nvs_set_* writes through to flash, so order the keys such that a failure
    // part way leaves flash LATCHED: on latch write the latch key first, on
    // clear write it last. (A cleared code with a set latch restores as a generic
    // latched fault — fail-safe.)
    if (latched) {
        err = nvs_set_u8(h, KEY_FAULT_LATCH, 1);
        if (err == ESP_OK) err = nvs_set_u8(h, KEY_FAULT_CODE, (uint8_t)code);
    } else {
        err = nvs_set_u8(h, KEY_FAULT_CODE, (uint8_t)code);
        if (err == ESP_OK) err = nvs_set_u8(h, KEY_FAULT_LATCH, 0);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "fault persist: %s", esp_err_to_name(err));
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
        esp_err_t pe = persist_fault(true, code);
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
    esp_err_t err = persist_fault(false, PB_FAULT_NONE);
    if (err != ESP_OK) {
        // A failure part way can leave the code key cleared. Let the tick retry
        // write the latch and its cause again.
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

void pb_heater_load_fault(void)
{
    nvs_handle_t h;
    esp_err_t oe = nvs_open(NVS_NS, NVS_READONLY, &h);
    bool ns_not_found = (oe == ESP_ERR_NVS_NOT_FOUND);
    bool open_ok      = (oe == ESP_OK);
    uint8_t latch_val = 0, code_val = PB_FAULT_NONE;
    bool latch_read_ok = false, latch_not_found = false;
    if (open_ok) {
        esp_err_t le = nvs_get_u8(h, KEY_FAULT_LATCH, &latch_val);
        latch_not_found = (le == ESP_ERR_NVS_NOT_FOUND);
        latch_read_ok   = (le == ESP_OK);
        nvs_get_u8(h, KEY_FAULT_CODE, &code_val);   // best-effort; decide() range-checks
        nvs_close(h);
    }
    pb_fault_reason_t code;
    bool latched = pb_heater_fault_decide(open_ok, ns_not_found, latch_read_ok,
                                          latch_not_found, latch_val, code_val, &code);
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

// --- First-heat check ------------------------------------------------------------

const char *pb_heater_first_heat_state_str(pb_fh_state_t s)
{
    switch (s) {
    case PB_FH_IDLE:    return "idle";
    case PB_FH_ARMED:   return "armed";
    case PB_FH_EVAL:    return "eval";
    case PB_FH_PASS:    return "pass";
    case PB_FH_WRONG_ROLES: return "wrong_roles";
    case PB_FH_NO_RISE: return "no_rise";
    case PB_FH_UNCERTAIN: return "uncertain";
    default:            return "unknown";
    }
}

const char *pb_heater_first_heat_stored_str(pb_fh_stored_t s)
{
    switch (s) {
    case PB_FH_STORED_PASS:    return "pass";
    case PB_FH_STORED_WRONG_ROLES: return "wrong_roles";
    case PB_FH_STORED_NO_RISE: return "no_rise";
    default:                   return "unknown";
    }
}

const char *pb_heater_first_heat_stock_str(pb_fh_stock_hint_t h)
{
    switch (h) {
    case PB_FH_STOCK_NONE:    return "none";
    case PB_FH_STOCK_UNKNOWN: return "unknown";
    case PB_FH_STOCK_CROSSED: return "crossed";
    case PB_FH_STOCK_NORMAL:  return "normal";
    default:                  return "invalid";
    }
}

// The thermistor mapping and the offsets (centi-degrees) that a stored result is
// valid for. A change of either makes the stored result UNKNOWN at the next boot.
static uint32_t fh_cfg_now(void)
{
    float oc = pb_ntc_get_offset_c(PB_NTC_CHAMBER), op = pb_ntc_get_offset_c(PB_NTC_PTC);
    int32_t c = (int32_t)(oc * 100.0f + (oc >= 0.0f ? 0.5f : -0.5f));
    int32_t p = (int32_t)(op * 100.0f + (op >= 0.0f ? 0.5f : -0.5f));
    if (c < -511) c = -511;
    if (c > 511) c = 511;
    if (p < -511) p = -511;
    if (p > 511) p = 511;
    return 0x80000000u | (pb_ntc_get_swap_thermistors_active() ? 1u : 0u) |
           ((uint32_t)(c + 512) << 1) | ((uint32_t)(p + 512) << 11);
}

static int32_t clamp_dc(int32_t v) { return v < -3000 ? -3000 : (v > 3000 ? 3000 : v); }

// Request an NVS write of the result. Caller holds s_mux.
static void fh_request_store_locked(pb_fh_stored_t val, int32_t ptc, int32_t ch, uint32_t cfg)
{
    s_fh_store_val = (uint8_t)val;
    s_fh_store_ptc = ptc;
    s_fh_store_ch = ch;
    s_fh_store_cfg = cfg;
    s_fh_store_seq++;
    s_fh_store_pending = true;
    s_fh_pub.stored = val;
}

// Write a pending result to NVS under the persist lock. The state key goes to
// UNKNOWN first and gets its value last, so a failure part way never leaves an
// old PASS with new data: the next boot runs the check again (fail-closed).
// wait=false (control task): returns ESP_ERR_TIMEOUT if another task holds the lock.
static esp_err_t fh_store(bool wait)
{
    if (s_persist_lock && xSemaphoreTake(s_persist_lock, wait ? portMAX_DELAY : 0) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    taskENTER_CRITICAL(&s_mux);
    bool pending = s_fh_store_pending;
    uint32_t seq = s_fh_store_seq;
    uint8_t val = s_fh_store_val;
    int32_t ptc = s_fh_store_ptc, ch = s_fh_store_ch;
    uint32_t cfg = s_fh_store_cfg;
    taskEXIT_CRITICAL(&s_mux);
    esp_err_t err = ESP_OK;
    if (pending) {
        nvs_handle_t h;
        err = nvs_open(NVS_NS, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_u8(h, KEY_FH_STATE, PB_FH_STORED_UNKNOWN);
            if (err == ESP_OK && val != PB_FH_STORED_UNKNOWN) {
                err = nvs_set_i32(h, KEY_FH_PTC, ptc);
                if (err == ESP_OK) err = nvs_set_i32(h, KEY_FH_CH, ch);
                if (err == ESP_OK) err = nvs_set_u32(h, KEY_FH_CFG, cfg);
                if (err == ESP_OK) err = nvs_set_u8(h, KEY_FH_STATE, val);
            }
            if (err == ESP_OK) err = nvs_commit(h);
            nvs_close(h);
        }
        if (err == ESP_OK) {
            taskENTER_CRITICAL(&s_mux);
            if (s_fh_store_seq == seq) s_fh_store_pending = false;   // no newer request
            taskEXIT_CRITICAL(&s_mux);
        }
    }
    bool warn = err != ESP_OK && !s_fh_store_warned;
    s_fh_store_warned = err != ESP_OK;
    if (s_persist_lock) xSemaphoreGive(s_persist_lock);
    if (warn) ESP_LOGW(TAG, "first-heat result not saved (will retry): %s", esp_err_to_name(err));
    return err;
}

// Read the stock app_nvs/ntc_bit. Stock writes a 4-byte blob. Other types and
// sizes are checked too, so a value of another layout gives INVALID, not a guess.
static pb_fh_stock_hint_t fh_read_stock(nvs_handle_t h)
{
    uint32_t v = 0;
    size_t len = 0;
    esp_err_t e = nvs_get_blob(h, KEY_STOCK_NTC_BIT, NULL, &len);
    if (e == ESP_OK) {
        uint8_t b[4] = {0};
        size_t got = len;
        if (len < 1 || len > sizeof b) return PB_FH_STOCK_INVALID;
        if (nvs_get_blob(h, KEY_STOCK_NTC_BIT, b, &got) != ESP_OK || got != len)
            return PB_FH_STOCK_INVALID;
        v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    } else if (e != ESP_ERR_NVS_NOT_FOUND) {
        return PB_FH_STOCK_INVALID;
    } else {
        uint8_t u8;
        int32_t i32;
        uint32_t u32;
        if (nvs_get_u8(h, KEY_STOCK_NTC_BIT, &u8) == ESP_OK)        v = u8;
        else if (nvs_get_i32(h, KEY_STOCK_NTC_BIT, &i32) == ESP_OK) v = (uint32_t)i32;
        else if (nvs_get_u32(h, KEY_STOCK_NTC_BIT, &u32) == ESP_OK) v = u32;
        else return PB_FH_STOCK_NONE;
    }
    switch (v) {
    case 0:  return PB_FH_STOCK_UNKNOWN;
    case 1:  return PB_FH_STOCK_CROSSED;
    case 2:  return PB_FH_STOCK_NORMAL;
    default: return PB_FH_STOCK_INVALID;
    }
}

void pb_heater_load_first_heat(void)
{
    uint8_t st = PB_FH_STORED_UNKNOWN;
    int32_t ptc = 0, ch = 0;
    uint32_t cfg = 0;
    bool st_ok = false, cfg_ok = false;
    pb_fh_stock_hint_t hint = PB_FH_STOCK_NONE;
    nvs_handle_t h;
    esp_err_t oe = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (oe == ESP_OK) {
        st_ok = nvs_get_u8(h, KEY_FH_STATE, &st) == ESP_OK;
        cfg_ok = nvs_get_u32(h, KEY_FH_CFG, &cfg) == ESP_OK;
        if (nvs_get_i32(h, KEY_FH_PTC, &ptc) != ESP_OK) ptc = 0;
        if (nvs_get_i32(h, KEY_FH_CH, &ch) != ESP_OK) ch = 0;
        hint = fh_read_stock(h);
        nvs_close(h);
    } else if (oe != ESP_ERR_NVS_NOT_FOUND) {
        hint = PB_FH_STOCK_INVALID;
    }
    // A read error, an unknown value or another mapping: UNKNOWN (run the check).
    pb_fh_stored_t stored = PB_FH_STORED_UNKNOWN;
    bool stale = false;
    if (st_ok && st >= PB_FH_STORED_PASS && st <= PB_FH_STORED_NO_RISE) {
        if (cfg_ok && cfg == fh_cfg_now()) stored = (pb_fh_stored_t)st;
        else stale = true;
    }
    if (stored == PB_FH_STORED_UNKNOWN) ptc = ch = 0;
    // A stored failure is shown by `stored` and by the restored fault latch. The
    // session starts IDLE, so the next heat after the clear runs the check.
    s_fh = stored == PB_FH_STORED_PASS ? PB_FH_PASS : PB_FH_IDLE;
    s_fh_ptc_rise = clamp_dc(ptc);
    s_fh_ch_rise = clamp_dc(ch);
    taskENTER_CRITICAL(&s_mux);
    s_fh_pub.state = s_fh;
    s_fh_pub.stored = stored;
    s_fh_pub.stock_hint = hint;
    s_fh_pub.ptc_rise_dc = s_fh_ptc_rise;
    s_fh_pub.ch_rise_dc = s_fh_ch_rise;
    s_fh_pub.on_ms = 0;
    if (stale) fh_request_store_locked(PB_FH_STORED_UNKNOWN, 0, 0, 0);
    taskEXIT_CRITICAL(&s_mux);
    if (stale) (void)fh_store(true);   // forget a result of another mapping (retried if it fails)
    ESP_LOGI(TAG, "first-heat: stored=%s%s stock_hint=%s", pb_heater_first_heat_stored_str(stored),
             stale ? " (reset: mapping or offsets changed)" : "", pb_heater_first_heat_stock_str(hint));
}

void pb_heater_get_first_heat(pb_heater_first_heat_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_mux);
    *out = s_fh_pub;
    taskEXIT_CRITICAL(&s_mux);
}

esp_err_t pb_heater_first_heat_rerun(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_fh_rerun_req = true;
    fh_request_store_locked(PB_FH_STORED_UNKNOWN, 0, 0, 0);
    taskEXIT_CRITICAL(&s_mux);
    esp_err_t err = fh_store(true);
    ESP_LOGI(TAG, "first-heat: re-run requested");
    return err;
}

static void fh_publish(void)   // control task
{
    taskENTER_CRITICAL(&s_mux);
    s_fh_pub.state = s_fh;
    s_fh_pub.ptc_rise_dc = s_fh_ptc_rise;
    s_fh_pub.ch_rise_dc = s_fh_ch_rise;
    s_fh_pub.on_ms = s_fh_on_us >= (int64_t)UINT32_MAX * 1000 ? UINT32_MAX
                                                               : (uint32_t)(s_fh_on_us / 1000);
    s_fh_pub.restarts = s_fh_restarts;
    s_fh_pub.skipped = s_fh_skipped;
    s_fh_pub.uncertain_streak = s_fh_uncertain_streak;
    taskEXIT_CRITICAL(&s_mux);
}

static bool fh_window_open(void) { return s_fh == PB_FH_ARMED || s_fh == PB_FH_EVAL; }

// A trip, a fault latch or a stop closes an open window without a result.
static void fh_abort(const char *why)
{
    (void)why;                       // unused if logging is compiled out
    if (!fh_window_open()) return;
    s_fh = PB_FH_IDLE;
    fh_publish();
    ESP_LOGI(TAG, "first-heat: window aborted (%s)", why);
}

// Call only while no fault is latched. A failed result stays while its latch
// holds. After the clear the check goes back to IDLE, so the next heat runs it
// again, also if the clear and a new target come before the next tick. A result
// without a latch (UNCERTAIN, or any fail in report-only mode) stays until the
// heat stops.
static void fh_release_fail(bool armed)
{
    bool fail = s_fh == PB_FH_WRONG_ROLES || s_fh == PB_FH_NO_RISE;
    if ((fail && (FH_ENFORCED || !armed)) || (s_fh == PB_FH_UNCERTAIN && !armed)) {
        s_fh = PB_FH_IDLE;
        fh_publish();
    }
}

// Close an open window without a result, count it as a restart. The next relay
// OFF->ON edge opens a new one.
static void fh_restart(const char *why)
{
    (void)why;                       // unused if logging is compiled out
    s_fh = PB_FH_IDLE;
    s_fh_restarts++;
    fh_publish();
    ESP_LOGI(TAG, "first-heat: window closed without a result (%s)", why);
}

// One step of an open window with valid readings. Latches a failed result
// itself (through trip()) and returns its fault code, or PB_FAULT_NONE.
static pb_fault_reason_t fh_step(float ptc_c, float ch_c, int64_t now)
{
    if (!fh_window_open()) return PB_FAULT_NONE;
    if (fh_cfg_now() != s_fh_cfg) {  // an offset changed: the readings moved
        fh_restart("offset changed");
        return PB_FAULT_NONE;
    }
    int64_t dt = now - s_fh_last_us;
    if (s_on && dt > 0) s_fh_on_us += dt;      // the relay was on since the last tick
    s_fh_last_us = now;
    int32_t p = pb_heater_c_to_dc(ptc_c), c = pb_heater_c_to_dc(ch_c);
    if (s_fh_nstart < 3) {
        // The start reading is the median of the first three readings.
        if (s_fh_nstart < 2) {
            s_fh_ptc_first[s_fh_nstart] = p;
            s_fh_ch_first[s_fh_nstart] = c;
            s_fh_nstart++;
            fh_publish();
            return PB_FAULT_NONE;
        }
        s_fh_ptc0 = pb_heater_median3(s_fh_ptc_first[0], s_fh_ptc_first[1], p);
        s_fh_ch0 = pb_heater_median3(s_fh_ch_first[0], s_fh_ch_first[1], c);
        s_fh_ptc_peak = s_fh_ptc0;
        s_fh_ch_peak = s_fh_ch0;
        s_fh_ptc_prev = s_fh_ptc_first[1];
        s_fh_ch_prev = s_fh_ch_first[1];
        s_fh_nstart = 3;
    }
    // Peak of min(this, last) reading: one glitch sample does not make a rise.
    int32_t pf = p < s_fh_ptc_prev ? p : s_fh_ptc_prev;
    int32_t cf = c < s_fh_ch_prev ? c : s_fh_ch_prev;
    s_fh_ptc_prev = p;
    s_fh_ch_prev = c;
    if (pf > s_fh_ptc_peak) s_fh_ptc_peak = pf;
    if (cf > s_fh_ch_peak) s_fh_ch_peak = cf;
    s_fh_ptc_rise = s_fh_ptc_peak - s_fh_ptc0;
    s_fh_ch_rise = s_fh_ch_peak - s_fh_ch0;
    uint32_t on_ms = s_fh_on_us >= (int64_t)UINT32_MAX * 1000 ? UINT32_MAX
                                                              : (uint32_t)(s_fh_on_us / 1000);
    int64_t wall_us = now - s_fh_t0_us;
    uint32_t wall_ms = wall_us >= (int64_t)UINT32_MAX * 1000 ? UINT32_MAX : (uint32_t)(wall_us / 1000);
    pb_fh_eval_t e = pb_heater_first_heat_eval(on_ms, wall_ms, s_fh_ptc_rise, s_fh_ch_rise);
    pb_fault_reason_t code = PB_FAULT_NONE;
    pb_fh_stored_t store = PB_FH_STORED_UNKNOWN;
    switch (e) {
    case PB_FH_EVAL_PASS:
        s_fh = PB_FH_PASS;
        store = PB_FH_STORED_PASS;
        break;
    case PB_FH_EVAL_WRONG_ROLES:
        s_fh = PB_FH_WRONG_ROLES;
        store = PB_FH_STORED_WRONG_ROLES;
        code = PB_FAULT_SENSOR_ROLES;
        break;
    case PB_FH_EVAL_NO_RISE:
        s_fh = PB_FH_NO_RISE;
        store = PB_FH_STORED_NO_RISE;
        code = PB_FAULT_HEATER_NO_RISE;
        break;
    case PB_FH_EVAL_UNCERTAIN:
        s_fh = PB_FH_UNCERTAIN;      // report only: no latch, nothing stored
        break;
    default:
        if (wall_ms > PB_FH_MAX_WALL_MS) {
            fh_restart("too little ON time");
            return PB_FAULT_NONE;
        }
        if (s_fh == PB_FH_ARMED && on_ms >= PB_FH_MIN_ON_MS) s_fh = PB_FH_EVAL;
        break;
    }
    if (e == PB_FH_EVAL_UNCERTAIN) s_fh_uncertain_streak++;        // a repeat stays visible
    else if (e != PB_FH_EVAL_WAIT) s_fh_uncertain_streak = 0;
    fh_publish();
    if (e == PB_FH_EVAL_WAIT) return PB_FAULT_NONE;
    ESP_LOGW(TAG, "first-heat: %s after %u ms ON: PTC rise %d dC, chamber rise %d dC%s",
             pb_heater_first_heat_state_str(s_fh), (unsigned)on_ms,
             (int)s_fh_ptc_rise, (int)s_fh_ch_rise,
             FH_ENFORCED ? "" : " (report only)");
    if (!FH_ENFORCED || e == PB_FH_EVAL_UNCERTAIN) return PB_FAULT_NONE;
    if (code != PB_FAULT_NONE) trip(code);     // relay off first, then the NVS record
    // A re-run that arrived during this tick wins: it already wrote UNKNOWN, and
    // the next tick closes this result. Do not store over it.
    taskENTER_CRITICAL(&s_mux);
    bool rerun = s_fh_rerun_req;
    if (!rerun) fh_request_store_locked(store, s_fh_ptc_rise, s_fh_ch_rise, s_fh_cfg);
    taskEXIT_CRITICAL(&s_mux);
    if (!rerun) {
        s_fh_retry_us = now;
        (void)fh_store(false);       // the tick retries a failed or contended write
    }
    return code;
}

// After the drive decision: a relay OFF->ON edge opens a window, unless the
// element is already warm (it can be at its equilibrium and cannot rise).
static void fh_after_drive(bool drive, bool was_on, float ptc_c, float ch_c, int64_t now)
{
    if (s_fh != PB_FH_IDLE || !drive || was_on) return;
    int32_t p = pb_heater_c_to_dc(ptc_c), c = pb_heater_c_to_dc(ch_c);
    if (pb_heater_first_heat_warm(p, c)) {
        s_fh_skipped++;
        fh_publish();
        return;
    }
    s_fh = PB_FH_ARMED;
    s_fh_t0_us = s_fh_last_us = now;
    s_fh_on_us = 0;
    s_fh_ptc_first[0] = p;
    s_fh_ch_first[0] = c;
    s_fh_nstart = 1;
    s_fh_ptc0 = s_fh_ptc_peak = p;
    s_fh_ch0 = s_fh_ch_peak = c;
    s_fh_ptc_rise = s_fh_ch_rise = 0;
    s_fh_cfg = fh_cfg_now();
    fh_publish();
    ESP_LOGI(TAG, "first-heat: window open (PTC %d dC, chamber %d dC)", (int)p, (int)c);
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
            esp_err_t pr = still ? persist_fault(true, code) : ESP_OK;  // cleared -> nothing to write
            if (pr == ESP_OK) {
                taskENTER_CRITICAL(&s_mux);
                s_persist_pending = false;
                taskEXIT_CRITICAL(&s_mux);
                ESP_LOGI(TAG, "deferred fault persist committed");
            }
            xSemaphoreGive(s_persist_lock);
        }
    }

    // First-heat: a re-run request from another task, and the retry of a result
    // write that failed (throttled like the fault persist retry).
    bool fh_rerun, fh_pending;
    taskENTER_CRITICAL(&s_mux);
    fh_rerun = s_fh_rerun_req;
    s_fh_rerun_req = false;
    fh_pending = s_fh_store_pending;
    taskEXIT_CRITICAL(&s_mux);
    if (fh_rerun && (s_fh == PB_FH_PASS || s_fh == PB_FH_UNCERTAIN || fh_window_open())) {
        s_fh = PB_FH_IDLE;               // a failed result waits for its clear
        s_fh_on_us = 0;
        s_fh_ptc_rise = s_fh_ch_rise = 0;
        fh_publish();
    }
    if (fh_pending) {
        int64_t now = esp_timer_get_time();
        if (now - s_fh_retry_us >= 2000000) {
            s_fh_retry_us = now;
            (void)fh_store(false);
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
        fh_abort("safety trip");     // the hazard trip wins over the first-heat check
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
        fh_abort(latched ? "fault latched" : "heat stopped");
        if (!latched) fh_release_fail(false);
        return;
    }

    // Here: armed && !latched && cs==OK && ps==OK.
    // First-heat check: it can only latch the heater off, through the same trip
    // path as the hazard checks above (see pb_heater.h).
    const int64_t now_us = esp_timer_get_time();
    fh_release_fail(true);
    if (fh_step(ptc_c, chamber_c, now_us) != PB_FAULT_NONE) return;   // latched in fh_step

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
        const bool bb_was_on = s_on;
        if (bb_drive != s_on) ssr_set(bb_drive);
        fh_after_drive(bb_drive, bb_was_on, ptc_c, chamber_c, now_us);
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
    else if (approach_limit < 1.0f)   // the soft approach cap is active (see pb_heater_pid_step)
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
    const bool was_on = s_on;
    if (drive != s_on) ssr_set(drive);
    fh_after_drive(drive, was_on, ptc_c, chamber_c, now_us);
}
