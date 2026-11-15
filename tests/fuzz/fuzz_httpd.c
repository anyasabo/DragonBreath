// libFuzzer harness for the DragonBreath HTTP API in pb_httpd.c. A LAN client
// can send each of these requests. The harness compiles the real pb_httpd.c and
// the real pb_policy.c. Fakes stand in for the heater, the sensors, the LEDs,
// dragon-core and ESP-IDF (fuzz_host.c and the headers in tests/fuzz/stubs).
//
// Input: 4 knob bytes (see fuzz_read_knobs), then requests in the format of
// fuzz_next_request(). The requests run one after the other on one device, so
// the fuzzer can find bugs that need a sequence (power_on, then heartbeat).
//
// Invariants, in addition to the ones in fuzz_run():
// - each target that reaches the heater is finite and in [0, max], and max <= 70,
// - each temperature setting that reaches pb_heater or pb_ntc is not NaN,
// - a 4xx response does not change the policy state revision,
// - a reboot task starts only while the heater is off and not armed.
// Build and run: tests/run_fuzz.sh.

#include "pb_httpd.c"   // static handlers and state; -I components/pb_httpd

#include "fuzz_host.h"
#include "pb_fan.h"

#include <assert.h>
#include <strings.h>

// ---- pb_heater fake ----------------------------------------------------------

static struct {
    float target_c, max_c, cool_release_c, fb_cut_c;
    uint32_t comms_ms;
    pb_heater_method_t method;
    bool latched, inhibited;
} H;

static void heater_check(void)
{
    FUZZ_CHECK(isfinite(H.max_c) && H.max_c >= PB_HEATER_MIN_TARGET_C
               && H.max_c <= PB_HEATER_ABS_MAX_TARGET_C, "heater max out of range");
    FUZZ_CHECK(isfinite(H.target_c) && H.target_c >= 0.0f && H.target_c <= H.max_c,
               "heater target out of range");
    FUZZ_CHECK(!(H.latched || H.inhibited) || H.target_c == 0.0f,
               "heater target above 0 while the fault latch is set");
}

esp_err_t pb_heater_set_target_c(float t)
{
    FUZZ_CHECK(isfinite(t), "non-finite heater target");
    if (t < 0.0f) t = 0.0f;
    if (t > H.max_c) t = H.max_c;
    if ((H.latched || H.inhibited) && t > 0.0f) return ESP_ERR_INVALID_STATE;
    if (t != H.target_c) fuzz_mutation("heater target");
    H.target_c = t;
    heater_check();
    return ESP_OK;
}

float pb_heater_get_target_c(void) { return H.target_c; }

esp_err_t pb_heater_set_max_target_c(float m)
{
    FUZZ_CHECK(!isnan(m), "NaN max target");
    if (!isfinite(m)) return ESP_ERR_INVALID_ARG;
    if (m < PB_HEATER_MIN_TARGET_C) m = PB_HEATER_MIN_TARGET_C;
    if (m > PB_HEATER_ABS_MAX_TARGET_C) m = PB_HEATER_ABS_MAX_TARGET_C;
    H.max_c = m;
    if (H.target_c > m) H.target_c = m;
    fuzz_mutation("max target");
    heater_check();
    return ESP_OK;
}

float pb_heater_get_max_target_c(void) { return H.max_c; }

esp_err_t pb_heater_set_comms_timeout_ms(uint32_t ms)
{
    if (ms < PB_HEATER_COMMS_TIMEOUT_MS_MIN) ms = PB_HEATER_COMMS_TIMEOUT_MS_MIN;
    if (ms > PB_HEATER_COMMS_TIMEOUT_MS_MAX) ms = PB_HEATER_COMMS_TIMEOUT_MS_MAX;
    H.comms_ms = ms;
    fuzz_mutation("comms timeout");
    return ESP_OK;
}

uint32_t pb_heater_get_comms_timeout_ms(void) { return H.comms_ms; }

esp_err_t pb_heater_set_cool_release_c(float c)
{
    FUZZ_CHECK(!isnan(c), "NaN cool_release");
    if (!isfinite(c)) return ESP_ERR_INVALID_ARG;
    if (c < PB_HEATER_COOL_RELEASE_MIN_C) c = PB_HEATER_COOL_RELEASE_MIN_C;
    if (c > PB_HEATER_COOL_RELEASE_MAX_C) c = PB_HEATER_COOL_RELEASE_MAX_C;
    H.cool_release_c = c;
    fuzz_mutation("cool_release");
    return ESP_OK;
}

float pb_heater_get_cool_release_c(void) { return H.cool_release_c; }

esp_err_t pb_heater_set_fb_cut_c(float c)
{
    FUZZ_CHECK(!isnan(c), "NaN fb_cut");
    if (!isfinite(c)) return ESP_ERR_INVALID_ARG;
    if (c <= 0.0f) c = 0.0f;
    else if (c < PB_HEATER_FB_CUT_MIN_C) c = PB_HEATER_FB_CUT_MIN_C;
    else if (c > PB_HEATER_FB_CUT_MAX_C) c = PB_HEATER_FB_CUT_MAX_C;
    H.fb_cut_c = c;
    fuzz_mutation("fb_cut");
    return ESP_OK;
}

float pb_heater_get_fb_cut_c(void) { return H.fb_cut_c; }

esp_err_t pb_heater_set_method(pb_heater_method_t m)
{
    FUZZ_CHECK(m == PB_HEATER_METHOD_BANGBANG || m == PB_HEATER_METHOD_PID, "bad heater method");
    H.method = m;
    fuzz_mutation("heater method");
    return ESP_OK;
}

pb_heater_method_t pb_heater_get_method(void) { return H.method; }

esp_err_t pb_heater_clear_fault(void)
{
    // Persist-first, as the real heater: a failed NVS write keeps the latch.
    nvs_handle_t h;
    if (nvs_open("app_nvs", NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    esp_err_t err = nvs_set_u8(h, "heat_fault", 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    H.latched = false;
    return ESP_OK;
}

void pb_heater_emergency_off(const char *reason)
{
    (void)reason;
    FUZZ_CHECK(!fuzz_in_request(), "emergency_off from an HTTP handler (not the control task)");
    H.latched = true;
    H.target_c = 0.0f;
}

void pb_heater_request_panic_off(const char *reason) { (void)reason; H.latched = true; H.target_c = 0.0f; }
void pb_heater_get_telemetry(pb_heater_telemetry_t *out) { memset(out, 0, sizeof *out); }
const char *pb_heater_constraint_str(pb_heater_constraint_t c) { (void)c; return "none"; }
void pb_heater_set_control_chamber_c(float c) { FUZZ_CHECK(!isinf(c), "infinite control temperature"); }
void pb_heater_notify_link_alive(void) {}
void pb_heater_tick(void) {}
void pb_heater_load_config(void) {}
bool pb_heater_is_inhibited(void) { return H.inhibited; }
bool pb_heater_is_faulted(void) { return H.latched; }
const char *pb_heater_fault_reason(void) { return H.latched ? "fuzz fault" : NULL; }
bool pb_heater_is_on(void) { return H.target_c > 0.0f && !H.latched; }
bool pb_heater_heat_mode(void) { return H.target_c > 0.0f; }

// ---- first-heat fakes (fork) -------------------------------------------------------

static pb_heater_first_heat_t s_fh;

void pb_heater_get_first_heat(pb_heater_first_heat_t *out) { *out = s_fh; }

// As the real one: the RAM reset applies, and a failed NVS write returns the error.
esp_err_t pb_heater_first_heat_rerun(void)
{
    s_fh.stored = PB_FH_STORED_UNKNOWN;
    s_fh.state = PB_FH_IDLE;
    fuzz_mutation("first-heat rerun");
    nvs_handle_t h;
    esp_err_t err = nvs_open("app_nvs", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "fh_res", 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

const char *pb_heater_first_heat_state_str(pb_fh_state_t st) { return st == PB_FH_PASS ? "pass" : "idle"; }
const char *pb_heater_first_heat_stored_str(pb_fh_stored_t st)
{
    return st == PB_FH_STORED_WRONG_ROLES ? "wrong_roles" : "unknown";
}
const char *pb_heater_first_heat_stock_str(pb_fh_stock_hint_t h) { (void)h; return "none"; }

// ---- pb_metrics fakes (fork) ----------------------------------------------------------

unsigned pb_metrics_reset_reason(void) { return 1; }   // ESP_RST_POWERON

esp_err_t pb_httpd_metrics_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; version=0.0.4");
    return httpd_resp_sendstr(req, "dragonbreath_up 1\n");
}

// ---- pb_ntc, pb_leds, pb_fan fakes ------------------------------------------------

static struct {
    float offset[2];
    float temp_c[2];
    pb_ntc_status_t status[2];
    bool swap;
} N;

static esp_err_t persist_u32(const char *key, uint32_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("app_nvs", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(h, key, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t pb_ntc_set_offset_c(pb_ntc_channel_t ch, float off)
{
    FUZZ_CHECK(ch == PB_NTC_CHAMBER || ch == PB_NTC_PTC, "bad NTC channel");
    FUZZ_CHECK(!isnan(off), "NaN calibration offset");
    float c = pb_ntc_clamp_offset_c(off);
    esp_err_t err = persist_u32(ch == PB_NTC_CHAMBER ? "ntc_off_ch" : "ntc_off_ptc",
                                (uint32_t)pb_ntc_clamp_offset_centi((int)lroundf(c * 100.0f)));
    if (err != ESP_OK) return err;
    N.offset[ch] = c;
    return ESP_OK;
}

float pb_ntc_get_offset_c(pb_ntc_channel_t ch) { return N.offset[ch]; }
pb_ntc_status_t pb_ntc_last_status(pb_ntc_channel_t ch) { return N.status[ch]; }
float pb_ntc_smoothed_c(pb_ntc_channel_t ch) { return N.status[ch] == PB_NTC_OK ? N.temp_c[ch] + N.offset[ch] : NAN; }
int pb_ntc_rref_kohm(void) { return 82; }
bool pb_ntc_get_swap_thermistors(void) { return N.swap; }
bool pb_ntc_get_swap_thermistors_active(void) { return false; }

esp_err_t pb_ntc_set_swap_thermistors(bool swap)
{
    esp_err_t err = persist_u32("ntc_swap", swap);
    if (err == ESP_OK) N.swap = swap;
    return err;
}

static bool s_leds_enabled = true;
static pb_led_pattern_t s_led[PB_LED_COUNT];

esp_err_t pb_leds_set_enabled(bool on)
{
    esp_err_t err = persist_u32("leds_en", on);
    if (err == ESP_OK) s_leds_enabled = on;
    return err;
}

bool pb_leds_get_enabled(void) { return s_leds_enabled; }
void pb_leds_set(pb_led_id_t id, pb_led_pattern_t p) { FUZZ_CHECK(id < PB_LED_COUNT, "bad LED"); s_led[id] = p; }
pb_led_pattern_t pb_leds_get(pb_led_id_t id) { FUZZ_CHECK(id < PB_LED_COUNT, "bad LED"); return s_led[id]; }
void pb_leds_set_code_once(pb_led_id_t id, uint8_t pulses) { (void)pulses; FUZZ_CHECK(id < PB_LED_COUNT, "bad LED"); }

static uint8_t s_fan;
void pb_fan_set_level(uint8_t percent) { FUZZ_CHECK(percent <= 100, "fan level above 100 %"); s_fan = percent; }
uint8_t pb_fan_get_level(void) { return s_fan; }

// ---- dragon-core fakes ----------------------------------------------------------------

static dc_ctl_source_t s_source;

dc_ctl_source_t dc_source_get(void) { return s_source; }
esp_err_t dc_source_set(dc_ctl_source_t src) { s_source = src; return ESP_OK; }
const char *dc_source_str(dc_ctl_source_t src) { (void)src; return "klipper"; }
bool dc_source_has_filament(dc_ctl_source_t src) { return src != DC_SRC_NONE && src != DC_SRC_HA; }

esp_err_t dc_bambu_get_status(dc_bambu_status_t *out)
{
    memset(out, 0, sizeof *out);
    out->chamber_temp = NAN;
    out->chamber_temp_age_ms = UINT32_MAX;
    out->printing = true;
    snprintf(out->filament, sizeof out->filament, "PETG");
    return ESP_OK;
}

esp_err_t dc_moonraker_get_status(dc_moonraker_status_t *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->material, sizeof out->material, "ABS");
    return ESP_OK;
}

esp_err_t dc_prusa_get_status(dc_prusa_status_t *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->material, sizeof out->material, "ASA");
    return ESP_OK;
}

// The zone table works like dc_bambu.c v0.36.0: 6 built-in types, up to 8
// custom profiles, names shorter than 12 characters, targets clamped to 70.
static const char *const ZONE_NAMES[DC_BAMBU_ZONE_COUNT] = { "PLA", "PETG", "ABS", "ASA", "PC", "PA" };
static dc_bambu_zone_t s_zone[DC_BAMBU_ZONE_MAX];
static int s_custom_n;

static void zones_reset(void)
{
    memset(s_zone, 0, sizeof s_zone);
    for (int i = 0; i < DC_BAMBU_ZONE_COUNT; i++) {
        snprintf(s_zone[i].name, sizeof s_zone[i].name, "%s", ZONE_NAMES[i]);
        s_zone[i].target_c = s_zone[i].default_c = (uint8_t)(i < 2 ? 0 : 45);
    }
    s_custom_n = 0;
}

int dc_bambu_zone_get_all(dc_bambu_zone_t *out, int max)
{
    int n = DC_BAMBU_ZONE_COUNT + s_custom_n;
    if (n > max) n = max;
    memcpy(out, s_zone, (size_t)n * sizeof *out);
    return n;
}

esp_err_t dc_bambu_zone_set(const char *name, uint8_t target_c)
{
    FUZZ_CHECK(name != NULL, "NULL zone name");
    if (!name[0]) return ESP_ERR_INVALID_ARG;
    if (target_c > 70) target_c = 70;
    int n = DC_BAMBU_ZONE_COUNT + s_custom_n;
    for (int i = 0; i < n; i++)
        if (!strcasecmp(name, s_zone[i].name)) {
            s_zone[i].target_c = target_c;
            fuzz_mutation("zone");
            return ESP_OK;
        }
    if (s_custom_n >= DC_BAMBU_CUSTOM_MAX) return ESP_ERR_NO_MEM;
    if (strlen(name) >= sizeof s_zone[0].name) return ESP_ERR_INVALID_SIZE;
    dc_bambu_zone_t *z = &s_zone[n];
    snprintf(z->name, sizeof z->name, "%s", name);
    z->target_c = target_c;
    z->custom = true;
    s_custom_n++;
    fuzz_mutation("zone");
    return ESP_OK;
}

esp_err_t dc_bambu_zone_remove(const char *name)
{
    FUZZ_CHECK(name != NULL, "NULL zone name");
    for (int i = DC_BAMBU_ZONE_COUNT; i < DC_BAMBU_ZONE_COUNT + s_custom_n; i++)
        if (!strcasecmp(name, s_zone[i].name)) {
            memmove(&s_zone[i], &s_zone[i + 1],
                    (size_t)(DC_BAMBU_ZONE_COUNT + s_custom_n - i - 1) * sizeof s_zone[0]);
            s_custom_n--;
            fuzz_mutation("zone remove");
            return ESP_OK;
        }
    return ESP_ERR_NOT_FOUND;
}

void dc_evlog_add(const char *fmt, ...) { (void)fmt; }

size_t dc_evlog_snapshot(dc_evlog_entry_t *out, size_t max)
{
    FUZZ_CHECK(max >= 1, "empty event log buffer");
    out[0].ms = 1;
    snprintf(out[0].text, sizeof out[0].text, "boot \"quoted\" \\ text");
    return 1;
}

size_t dc_evlog_console_snapshot(char *out, size_t max)
{
    FUZZ_CHECK(max >= 1, "empty console buffer");
    memset(out, 'x', max - 1);   // a full ring
    out[max - 1] = '\0';
    return max - 1;
}

// ---- ESP-IDF fakes ------------------------------------------------------------------------

static uint32_t s_random;
uint32_t esp_random(void) { return s_random += 0x9e3779b9u; }

void esp_fill_random(void *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) ((uint8_t *)buf)[i] = (uint8_t)esp_random();
}

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    (void)type;
    memcpy(mac, "\x24\x0a\xc4\x01\x02\x03", 6);
    return ESP_OK;
}

static const esp_app_desc_t s_app = { .version = "fuzz", .project_name = "dragonbreath" };
const esp_app_desc_t *esp_app_get_description(void) { return &s_app; }

static const esp_partition_t s_ota1 = { .label = "ota_1" };
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start) { (void)start; return &s_ota1; }

esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *out)
{
    (void)p;
    memset(out, 0, sizeof *out);
    snprintf(out->project_name, sizeof out->project_name, "panda_breath");
    snprintf(out->version, sizeof out->version, "1.0.0");
    return ESP_OK;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{
    (void)p;
    fuzz_mutation("boot partition");
    return ESP_OK;
}

// No setup AP: a client is never on the setup AP (require-token build).
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *if_key) { (void)if_key; return NULL; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info)
{
    (void)netif;
    memset(info, 0, sizeof *info);
    return ESP_ERR_INVALID_STATE;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) { ap->rssi = -50; ap->primary = 6; return ESP_OK; }
uint32_t esp_get_free_heap_size(void) { return 100000; }
uint32_t esp_get_minimum_free_heap_size(void) { return 90000; }
void esp_restart(void) { fuzz_fail(__FILE__, __LINE__, "esp_restart() from a handler"); }

// ---- harness ------------------------------------------------------------------------------------

static void device_reset(const fuzz_knobs_t *k)
{
    memset(&H, 0, sizeof H);
    H.max_c = PB_HEATER_ABS_MAX_TARGET_C;
    H.cool_release_c = PB_HEATER_COOL_RELEASE_C_DEFAULT;
    H.comms_ms = PB_HEATER_COMMS_TIMEOUT_MS_DEFAULT;
    H.latched = k->device[0] & 0x02;
    H.inhibited = k->device[0] & 0x04;
    memset(&N, 0, sizeof N);
    N.temp_c[PB_NTC_CHAMBER] = 15.0f + (float)(k->device[1] & 0x3f);
    N.temp_c[PB_NTC_PTC] = 20.0f + (float)(k->device[1] >> 1);
    N.status[PB_NTC_CHAMBER] = (k->device[0] & 0x08) ? PB_NTC_OPEN : PB_NTC_OK;
    N.status[PB_NTC_PTC] = PB_NTC_OK;
    s_leds_enabled = true;
    memset(s_led, 0, sizeof s_led);
    s_fan = 0;
    s_source = (dc_ctl_source_t)((k->device[0] >> 4) % DC_SRC_MAX);
    s_random = 0;
    zones_reset();
    memset(&s_fh, 0, sizeof s_fh);
    s_fh.stored = (pb_fh_stored_t)(k->device[1] & 0x03);
    s_fh.enforced = true;

    // A configured control token: then the auth header must match it.
    if (k->device[0] & 0x01) fuzz_nvs_put_str("ctl_token", "fuzz-token");

    // State of pb_httpd.c from earlier inputs.
    memset(s_replay, 0, sizeof s_replay);
    s_replay_next = 0;
    s_sse_clients = 0;

    pb_policy_init();
    pb_policy_load_params();
    pb_policy_set_control_source(s_source);
    FUZZ_CHECK(pb_httpd_register((httpd_handle_t)1) == ESP_OK, "pb_httpd_register failed");
    static const char *const routes[] = {
        "GET /api/v2/info", "GET /api/v2/state", "POST /api/v2/command",
        "POST /api/v2/heartbeat", "GET /api/v2/events", "GET /api/v2/health",
        "GET /settings", "POST /settings", "GET /api/v2/logs", "GET /api/v2/console",
        "POST /api/v2/restart", "POST /api/v2/boot-inactive", "POST /api/v2/factory-reset",
        "POST /api/v2/token", "GET /api/v2/calibration", "POST /api/v2/calibration",
        "GET /api/v2/zones", "POST /api/v2/zones", "GET /api/v2/heater_method",
        "POST /api/v2/heater_method", "GET /metrics",
    };
    fuzz_check_routes(routes, sizeof routes / sizeof routes[0]);
}

static bool device_armed_now(void)
{
    pb_policy_snapshot_t snap;
    pb_policy_get_snapshot(&snap);
    return snap.mode != PB_MODE_OFF || snap.heater_output || H.target_c > 0.0f;
}

static void control_tick(void)
{
    pb_policy_tick();
    // One write attempt for each tick, as the persist task does on each wake.
    // With failed NVS writes, the parameters stay dirty and it tries again.
    (void)pb_policy_persist_pending();
    heater_check();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_knobs_t knobs;
    size_t used = fuzz_read_knobs(data, size, &knobs);
    data += used;
    size -= used;
    fuzz_host_reset(&knobs);
    device_reset(&knobs);

    fuzz_request_t req;
    for (int n = 0; n < 16 && fuzz_next_request(&data, &size, &req); n++) {
        if (req.wait_s) {
            for (uint32_t s = 0; s < req.wait_s; s += 5) {
                fuzz_advance_ms(5000);
                control_tick();
            }
            continue;
        }
        const fuzz_route_t *route = fuzz_find_route(req.method, req.uri);
        if (!route) continue;

        pb_policy_snapshot_t before;
        pb_policy_get_snapshot(&before);
        bool armed = device_armed_now();
        unsigned mutations = fuzz_mutation_count();

        fuzz_response_t resp;
        fuzz_run(route, &req, &resp);

        pb_policy_snapshot_t after;
        pb_policy_get_snapshot(&after);
        if (resp.status >= 400 && resp.status < 500)
            FUZZ_CHECK(after.state_revision == before.state_revision,
                       "policy state changed, but the response is 4xx");
        if (armed && (!strcmp(req.uri, "/api/v2/restart")
                      || !strcmp(req.uri, "/api/v2/factory-reset")
                      || !strcmp(req.uri, "/api/v2/boot-inactive")))
            FUZZ_CHECK(fuzz_mutation_count() == mutations,
                       "maintenance action ran while the heater was armed");
        FUZZ_CHECK(isfinite(after.effective_target_c) || after.effective_target_c == 0.0f,
                   "non-finite effective target in the snapshot");
        fuzz_response_free(&resp);

        fuzz_advance_ms(500);
        control_tick();
    }
    return 0;
}
