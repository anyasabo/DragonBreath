// libFuzzer harness for the DragonBreath part of the setup portal in
// db_portal.c: the product settings form (apply_product), the dashboard
// presets and the other product routes. A LAN client can send each of these
// requests. The harness compiles the real db_portal.c and db_portal_config.c.
// dc_portal (dragon-core) parses the form body and calls apply_product(). Small
// stand-ins below copy that behaviour of dc_portal v0.36.0, with the body
// limits of dragon-core fix/portal-body-read.
//
// Input: 4 knob bytes (see fuzz_read_knobs), then requests in the format of
// fuzz_next_request().
//
// Invariants, in addition to the ones in fuzz_run():
// - apply_product() changes nothing if it returns an error that is not a
//   persistence error (the form is all or nothing),
// - each config that reaches a setter has terminated strings, a port that is
//   not 0, and a valid source and hostname,
// - the presets in NVS are 4 values of at most 70 C,
// - a factory reset runs only while the heater is off.
// Build and run: tests/run_fuzz.sh.

#include "db_portal.h"
#include "db_portal_config.h"
#include "dc_portal.h"
#include "dc_wifi.h"
#include "esp_timer.h"
#include "fuzz_host.h"
#include "nvs.h"
#include "pb_httpd.h"
#include "pb_policy.h"

#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TERMINATED(field) (memchr((field), '\0', sizeof(field)) != NULL)

static fuzz_knobs_t s_knobs;
static unsigned s_persist_failures;   // setter calls that returned an error

// A setter fails when the NVS write knob is set, like a real NVS write.
static esp_err_t setter_result(const char *what)
{
    if (s_knobs.nvs_write_fails) { s_persist_failures++; return ESP_FAIL; }
    fuzz_mutation(what);
    return ESP_OK;
}

// ---- config fakes --------------------------------------------------------------

static dc_moonraker_config_t s_mr;
static dc_bambu_config_t s_bb;
static pb_ha_config_t s_ha;
static db_km_config_t s_km;
static dc_prusa_config_t s_pr;
static dc_ctl_source_t s_source;
static char s_hostname[33];

esp_err_t dc_moonraker_get_config(dc_moonraker_config_t *out) { *out = s_mr; return ESP_OK; }
esp_err_t dc_bambu_get_config(dc_bambu_config_t *out) { *out = s_bb; return ESP_OK; }
esp_err_t pb_ha_get_config(pb_ha_config_t *out) { *out = s_ha; return ESP_OK; }
esp_err_t db_klipper_mqtt_get_config(db_km_config_t *out) { *out = s_km; return ESP_OK; }
esp_err_t dc_prusa_get_config(dc_prusa_config_t *out) { *out = s_pr; return ESP_OK; }

esp_err_t dc_moonraker_set_config(const dc_moonraker_config_t *c)
{
    FUZZ_CHECK(TERMINATED(c->host) && TERMINATED(c->api_key), "Moonraker config not terminated");
    esp_err_t err = setter_result("moonraker");
    if (err == ESP_OK) s_mr = *c;
    return err;
}

esp_err_t dc_bambu_set_config(const dc_bambu_config_t *c)
{
    FUZZ_CHECK(TERMINATED(c->host) && TERMINATED(c->serial) && TERMINATED(c->code),
               "Bambu config not terminated");
    esp_err_t err = setter_result("bambu");
    if (err == ESP_OK) s_bb = *c;
    return err;
}

esp_err_t pb_ha_set_config(const pb_ha_config_t *c)
{
    FUZZ_CHECK(TERMINATED(c->host) && TERMINATED(c->user) && TERMINATED(c->pass)
               && TERMINATED(c->topic), "Home Assistant config not terminated");
    esp_err_t err = setter_result("ha");
    if (err == ESP_OK) s_ha = *c;
    return err;
}

esp_err_t db_klipper_mqtt_set_config(const db_km_config_t *c)
{
    FUZZ_CHECK(TERMINATED(c->host) && TERMINATED(c->user) && TERMINATED(c->pass)
               && TERMINATED(c->inst) && TERMINATED(c->topic), "Klipper MQTT config not terminated");
    esp_err_t err = setter_result("klipper mqtt");
    if (err == ESP_OK) s_km = *c;
    return err;
}

esp_err_t dc_prusa_set_config(const dc_prusa_config_t *c)
{
    FUZZ_CHECK(TERMINATED(c->host) && TERMINATED(c->api_key), "Prusa config not terminated");
    esp_err_t err = setter_result("prusa");
    if (err == ESP_OK) s_pr = *c;
    return err;
}

dc_ctl_source_t dc_source_get(void) { return s_source; }

esp_err_t dc_source_set(dc_ctl_source_t src)
{
    FUZZ_CHECK((unsigned)src < DC_SRC_MAX, "control source out of range");
    esp_err_t err = setter_result("source");
    if (err == ESP_OK) s_source = src;
    return err;
}

// The same rule as dc_wifi_hostname_valid() in dragon-core v0.36.0.
bool dc_wifi_hostname_valid(const char *h)
{
    size_t n = h ? strlen(h) : 0;
    if (n < 1 || n > 32 || h[0] == '-' || h[n - 1] == '-') return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)h[i]) && h[i] != '-') return false;
    return true;
}

esp_err_t dc_wifi_get_hostname(char *out, size_t out_size)
{
    FUZZ_CHECK(out_size >= 33, "hostname buffer too small");
    snprintf(out, out_size, "%s", s_hostname);
    return ESP_OK;
}

esp_err_t dc_wifi_set_hostname(const char *h)
{
    FUZZ_CHECK(dc_wifi_hostname_valid(h), "invalid hostname reached dc_wifi_set_hostname");
    esp_err_t err = setter_result("hostname");
    if (err == ESP_OK) snprintf(s_hostname, sizeof s_hostname, "%s", h);
    return err;
}

// ---- other fakes ---------------------------------------------------------------------

static bool s_scanning;
esp_err_t dc_bambu_scan_start(void) { s_scanning = true; return ESP_OK; }
bool dc_bambu_scanning(void) { return s_scanning; }

int dc_bambu_discover_get(dc_bambu_found_t *out, int max)
{
    FUZZ_CHECK(max >= 1, "empty discovery buffer");
    memset(out, 0, sizeof *out);
    snprintf(out->host, sizeof out->host, "192.168.1.20");
    snprintf(out->serial, sizeof out->serial, "01P00A000000000");
    snprintf(out->model, sizeof out->model, "C12");
    snprintf(out->name, sizeof out->name, "P1S \"lab\"");
    return 1;
}

static bool s_armed;

void pb_policy_get_snapshot(pb_policy_snapshot_t *out)
{
    memset(out, 0, sizeof *out);
    out->mode = s_armed ? PB_MODE_POWER_ON : PB_MODE_OFF;
    out->heater_output = s_armed;
}

bool pb_httpd_auth_ok(httpd_req_t *req)
{
    // The real check is in fuzz_httpd. Here: the header must be present.
    return httpd_req_get_hdr_value_len(req, PB_AUTH_HEADER) > 0;
}

esp_err_t pb_httpd_register(httpd_handle_t server) { return server ? ESP_OK : ESP_ERR_INVALID_ARG; }

// A route handler as pb_httpd_register_route() (fix/httpd-body-read) and the
// dc_portal built-in routes (dragon-core fix/portal-body-read) run it: if body
// bytes stay unread after the handler, return ESP_FAIL. Then httpd closes the
// socket and does not read the rest of the body.
static httpd_uri_t s_wrapped[16];
static size_t s_wrapped_count;

static esp_err_t wrapped_route(httpd_req_t *req)
{
    const httpd_uri_t *route = req->user_ctx;
    req->user_ctx = route->user_ctx;
    esp_err_t err = route->handler(req);
    return err == ESP_OK && fuzz_body_consumed() < req->content_len ? ESP_FAIL : err;
}

esp_err_t pb_httpd_register_route(httpd_handle_t server, const httpd_uri_t *uri)
{
    FUZZ_CHECK(s_wrapped_count < sizeof s_wrapped / sizeof s_wrapped[0], "too many routes");
    httpd_uri_t *copy = &s_wrapped[s_wrapped_count++];
    *copy = *uri;
    httpd_uri_t wrapped = *uri;
    wrapped.handler = wrapped_route;
    wrapped.user_ctx = copy;
    return httpd_register_uri_handler(server, &wrapped);
}

// The bounded body read of pb_httpd_read_body() and dc_portal_read_body(): stop
// after 3 timeouts in a row with no data, or after 10 s in total.
static bool bounded_read(httpd_req_t *req, char *buf, size_t len)
{
    const int64_t start = esp_timer_get_time();
    size_t got = 0;
    int idle = 0;
    while (got < len) {
        if (esp_timer_get_time() - start > 10 * 1000000LL) return false;
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++idle >= 3) return false;
            continue;
        }
        if (r <= 0) return false;
        idle = 0;
        got += (size_t)r;
    }
    return true;
}

bool pb_httpd_read_body(httpd_req_t *req, char *buf, size_t len)
{
    return bounded_read(req, buf, len);
}

static const esp_app_desc_t s_app = { .version = "fuzz", .project_name = "dragonbreath" };
const esp_app_desc_t *esp_app_get_description(void) { return &s_app; }

// The firmware build embeds favicon.png with these two symbols. The end
// symbol must follow the data, so define both in one block.
__asm__(".section .rodata\n"
        ".globl _binary_favicon_png_start\n_binary_favicon_png_start:\n"
        ".byte 0x89, 0x50, 0x4e, 0x47\n"
        ".globl _binary_favicon_png_end\n_binary_favicon_png_end:\n"
        ".byte 0\n"
        ".previous\n");

// ---- dc_portal stand-ins (dragon-core v0.36.0 + fix/portal-body-read) ------------------

static dc_portal_config_t s_portal;

static esp_err_t send_reply(httpd_req_t *req, const char *status, cJSON *root)
{
    if (status) httpd_resp_set_status(req, status);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    FUZZ_CHECK(s != NULL, "out of memory");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    return err;
}

static esp_err_t json_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    return send_reply(req, status, root);
}

// The body read of dc_portal.c recv_json() (dragon-core fix/portal-body-read).
// dc_portal itself is out of scope.
static cJSON *portal_recv_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) return NULL;
    char *body = malloc(req->content_len + 1);
    FUZZ_CHECK(body != NULL, "out of memory");
    if (!bounded_read(req, body, req->content_len)) { free(body); return NULL; }
    body[req->content_len] = '\0';
    cJSON *root = cJSON_Parse(body);
    free(body);
    return root;
}

static esp_err_t product_post(httpd_req_t *req)
{
    if (!s_portal.authorize(req, s_portal.ctx))
        return json_error(req, "403 Forbidden", "authorization required");
    cJSON *body = portal_recv_json(req);
    cJSON *values = body ? cJSON_GetObjectItemCaseSensitive(body, "values") : NULL;
    if (!cJSON_IsObject(values)) {
        cJSON_Delete(body);
        return json_error(req, "400 Bad Request", "values object is required");
    }
    char message[128] = {0};
    unsigned before = fuzz_mutation_count();
    unsigned failures = s_persist_failures;
    esp_err_t err = s_portal.apply_product(values, s_portal.ctx, message, sizeof message);
    cJSON_Delete(body);
    FUZZ_CHECK(memchr(message, '\0', sizeof message) != NULL, "apply_product message not terminated");
    if (err != ESP_OK) {
        // A form error must come before the first setter. Only a failed save
        // can leave a part of the form saved.
        FUZZ_CHECK(s_persist_failures != failures || fuzz_mutation_count() == before,
                   "apply_product returned a form error after a setter ran");
        if (s_persist_failures != failures)
            return json_error(req, "500 Internal Server Error", message);
        return json_error(req, "400 Bad Request", message[0] ? message : "error");
    }
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", true);
    if (message[0]) cJSON_AddStringToObject(reply, "message", message);
    return send_reply(req, NULL, reply);
}

static esp_err_t provisioning_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *product = s_portal.describe_product(s_portal.ctx);
    if (product) cJSON_AddItemToObject(root, "product_setup", product);
    return send_reply(req, NULL, root);
}

static esp_err_t reset_post(httpd_req_t *req)
{
    if (!s_portal.authorize(req, s_portal.ctx))
        return json_error(req, "403 Forbidden", "authorization required");
    char message[128] = {0};
    if (s_portal.guard_operation(DC_PORTAL_OPERATION_FACTORY_RESET, s_portal.ctx,
                                 message, sizeof message) != ESP_OK)
        return json_error(req, "409 Conflict", message);
    FUZZ_CHECK(!s_armed, "factory reset allowed while the heater is armed");
    if (s_portal.factory_reset(s_portal.ctx) != ESP_OK)
        return json_error(req, "500 Internal Server Error", "reset failed");
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", true);
    return send_reply(req, NULL, reply);
}

esp_err_t dc_portal_start(const dc_portal_config_t *config)
{
    FUZZ_CHECK(config->apply_product && config->describe_product && config->authorize
               && config->guard_operation && config->factory_reset
               && config->register_product_routes, "db_portal left out a callback");
    s_portal = *config;
    static const httpd_uri_t own[] = {
        { .uri = "/api/v1/provisioning", .method = HTTP_GET, .handler = provisioning_get },
        { .uri = "/api/v1/provisioning/product", .method = HTTP_POST, .handler = product_post },
        { .uri = "/api/v1/system/reset", .method = HTTP_POST, .handler = reset_post },
    };
    for (size_t i = 0; i < sizeof own / sizeof own[0]; i++)
        pb_httpd_register_route((httpd_handle_t)1, &own[i]);
    return config->register_product_routes((httpd_handle_t)1, config->ctx);
}

// ---- harness ------------------------------------------------------------------------------

static void presets_check(void)
{
    nvs_handle_t h;
    if (nvs_open("app_nvs", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t p[8];
    size_t len = sizeof p;
    esp_err_t err = nvs_get_blob(h, "quick_preset", p, &len);
    nvs_close(h);
    if (err != ESP_OK) return;
    FUZZ_CHECK(len == 4, "presets blob is not 4 bytes");
    for (size_t i = 0; i < len; i++) FUZZ_CHECK(p[i] <= 70, "preset above 70 C");
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    size_t used = fuzz_read_knobs(data, size, &s_knobs);
    data += used;
    size -= used;
    fuzz_host_reset(&s_knobs);

    memset(&s_mr, 0, sizeof s_mr);
    memset(&s_bb, 0, sizeof s_bb);
    memset(&s_ha, 0, sizeof s_ha);
    memset(&s_km, 0, sizeof s_km);
    memset(&s_pr, 0, sizeof s_pr);
    if (s_knobs.device[0] & 0x01) {
        // A device that is set up already: empty secret fields keep these.
        snprintf(s_mr.host, sizeof s_mr.host, "printer.local");
        s_mr.port = 7125;
        snprintf(s_km.host, sizeof s_km.host, "printer.local");
        snprintf(s_km.user, sizeof s_km.user, "dragonbreath");
        snprintf(s_km.pass, sizeof s_km.pass, "secret");
        snprintf(s_km.inst, sizeof s_km.inst, "printer");
        snprintf(s_pr.host, sizeof s_pr.host, "192.168.1.30");
        snprintf(s_pr.api_key, sizeof s_pr.api_key, "prusalink-key");
    }
    s_source = (dc_ctl_source_t)((s_knobs.device[0] >> 4) % DC_SRC_MAX);
    snprintf(s_hostname, sizeof s_hostname, "dragonbreath");
    s_armed = s_knobs.device[0] & 0x02;
    s_scanning = false;
    s_persist_failures = 0;
    s_wrapped_count = 0;
    FUZZ_CHECK(db_portal_start() == ESP_OK, "db_portal_start failed");

    fuzz_request_t req;
    for (int n = 0; n < 16 && fuzz_next_request(&data, &size, &req); n++) {
        if (req.wait_s) continue;
        const fuzz_route_t *route = fuzz_find_route(req.method, req.uri);
        if (!route) continue;
        fuzz_response_t resp;
        fuzz_run(route, &req, &resp);
        fuzz_response_free(&resp);
        presets_check();
    }
    return 0;
}
