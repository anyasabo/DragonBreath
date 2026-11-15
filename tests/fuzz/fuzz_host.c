// Shared host fakes for the HTTP handler fuzz harnesses. See fuzz_host.h.
#include "fuzz_host.h"

#include "cJSON.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_URI_LEN 512        // CONFIG_HTTPD_MAX_URI_LEN default
#define STALL_LIMIT 64         // timeouts for one stalled request before we stop
#define RECV_WAIT_US 5000000   // recv_wait_timeout of HTTPD_DEFAULT_CONFIG()
#define DRIP_US      4900000   // a drip client sends 1 byte just before the timeout
#define PURGE_BUF_LEN 32       // CONFIG_HTTPD_PURGE_BUF_LEN default

void fuzz_fail(const char *file, int line, const char *what)
{
    fprintf(stderr, "%s:%d: invariant broken: %s\n", file, line, what);
    abort();
}

const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "ERROR";
}

// ---- time --------------------------------------------------------------------

static int64_t s_now_us;

int64_t esp_timer_get_time(void) { return s_now_us; }

void fuzz_advance_ms(uint32_t ms) { s_now_us += (int64_t)ms * 1000; }

// ---- state of one input ------------------------------------------------------

static fuzz_knobs_t s_knobs;
static uint32_t s_rng;
static unsigned s_mutations;

static uint32_t rng_next(void)
{
    // xorshift32: the chunk sizes are a function of the input only.
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s_rng = x;
}

void fuzz_mutation(const char *what)
{
    (void)what;
    s_mutations++;
}

unsigned fuzz_mutation_count(void) { return s_mutations; }

size_t fuzz_read_knobs(const uint8_t *data, size_t size, fuzz_knobs_t *k)
{
    memset(k, 0, sizeof *k);
    if (size < 4) return size;
    k->seed = data[0];
    k->nvs_write_fails = data[1] & 0x01;
    k->recv_timeouts = data[1] & 0x02;
    k->client_stalls = data[1] & 0x04;
    k->client_drips = data[1] & 0x08;
    k->device[0] = data[2];
    k->device[1] = data[3];
    return 4;
}

// ---- NVS -----------------------------------------------------------------------

#define NVS_MAX_ENTRIES 64
#define NVS_STR_MAX     4000   // ESP-IDF limit for a string, with the NUL

typedef enum { NV_STR, NV_BLOB, NV_U8, NV_U32 } nv_type_t;

typedef struct {
    bool used;
    char ns[16];
    char key[16];
    nv_type_t type;
    size_t len;
    uint8_t data[NVS_STR_MAX];
} nv_entry_t;

static nv_entry_t s_nvs[NVS_MAX_ENTRIES];
static char s_open_ns[8][16];
static bool s_open_rw[8];

static const char *handle_ns(nvs_handle_t h)
{
    FUZZ_CHECK(h >= 1 && h <= 8 && s_open_ns[h - 1][0], "NVS use of a closed handle");
    return s_open_ns[h - 1];
}

static nv_entry_t *nv_find(const char *ns, const char *key)
{
    for (int i = 0; i < NVS_MAX_ENTRIES; i++)
        if (s_nvs[i].used && !strcmp(s_nvs[i].ns, ns) && !strcmp(s_nvs[i].key, key))
            return &s_nvs[i];
    return NULL;
}

static esp_err_t nv_put(nvs_handle_t h, const char *key, nv_type_t type,
                        const void *data, size_t len)
{
    const char *ns = handle_ns(h);
    FUZZ_CHECK(s_open_rw[h - 1], "NVS write on a read-only handle");
    FUZZ_CHECK(key && strlen(key) <= 15, "NVS key longer than 15 characters");
    FUZZ_CHECK(len <= NVS_STR_MAX, "NVS value too long");
    if (s_knobs.nvs_write_fails) return ESP_FAIL;
    nv_entry_t *e = nv_find(ns, key);
    for (int i = 0; !e && i < NVS_MAX_ENTRIES; i++)
        if (!s_nvs[i].used) e = &s_nvs[i];
    FUZZ_CHECK(e != NULL, "NVS store full");
    e->used = true;
    snprintf(e->ns, sizeof e->ns, "%s", ns);
    snprintf(e->key, sizeof e->key, "%s", key);
    e->type = type;
    e->len = len;
    memcpy(e->data, data, len);
    fuzz_mutation("nvs write");
    return ESP_OK;
}

static esp_err_t nv_get(nvs_handle_t h, const char *key, nv_type_t type,
                        void *out, size_t *len)
{
    nv_entry_t *e = nv_find(handle_ns(h), key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (e->type != type) return ESP_ERR_NVS_TYPE_MISMATCH;
    if (!out) { *len = e->len; return ESP_OK; }
    if (*len < e->len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    FUZZ_CHECK(ns && strlen(ns) <= 15, "NVS namespace longer than 15 characters");
    for (int i = 0; i < 8; i++) {
        if (s_open_ns[i][0]) continue;
        snprintf(s_open_ns[i], sizeof s_open_ns[i], "%s", ns);
        s_open_rw[i] = mode == NVS_READWRITE;
        *out = (nvs_handle_t)(i + 1);
        return ESP_OK;
    }
    fuzz_fail(__FILE__, __LINE__, "NVS handle leak: more than 8 open handles");
}

void nvs_close(nvs_handle_t h)
{
    handle_ns(h);
    s_open_ns[h - 1][0] = '\0';
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    handle_ns(h);
    return s_knobs.nvs_write_fails ? ESP_FAIL : ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    FUZZ_CHECK(s_open_rw[h - 1], "NVS erase on a read-only handle");
    nv_entry_t *e = nv_find(handle_ns(h), key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (s_knobs.nvs_write_fails) return ESP_FAIL;
    e->used = false;
    fuzz_mutation("nvs erase");
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    const char *ns = handle_ns(h);
    FUZZ_CHECK(s_open_rw[h - 1], "NVS erase on a read-only handle");
    if (s_knobs.nvs_write_fails) return ESP_FAIL;
    for (int i = 0; i < NVS_MAX_ENTRIES; i++)
        if (s_nvs[i].used && !strcmp(s_nvs[i].ns, ns)) s_nvs[i].used = false;
    fuzz_mutation("nvs erase all");
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    // The string must end inside the NVS limit. memchr() reads no further.
    FUZZ_CHECK(value && memchr(value, '\0', NVS_STR_MAX), "NVS string not terminated");
    return nv_put(h, key, NV_STR, value, strlen(value) + 1);
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    return nv_get(h, key, NV_STR, out, len);
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len)
{
    return nv_put(h, key, NV_BLOB, value, len);
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    return nv_get(h, key, NV_BLOB, out, len);
}

esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value)
{
    return nv_put(h, key, NV_U8, &value, 1);
}

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    size_t len = 1;
    return nv_get(h, key, NV_U8, out, &len);
}

esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value)
{
    return nv_put(h, key, NV_U32, &value, 4);
}

esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    size_t len = 4;
    return nv_get(h, key, NV_U32, out, &len);
}

void fuzz_nvs_put_str(const char *key, const char *value)
{
    nv_entry_t *e = &s_nvs[NVS_MAX_ENTRIES - 1];
    e->used = true;
    snprintf(e->ns, sizeof e->ns, "app_nvs");
    snprintf(e->key, sizeof e->key, "%s", key);
    e->type = NV_STR;
    e->len = strlen(value) + 1;
    memcpy(e->data, value, e->len);
}

// ---- tasks -----------------------------------------------------------------------

static bool s_in_sse;
static unsigned s_sse_events;

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, unsigned prio, TaskHandle_t *out)
{
    (void)stack; (void)prio;
    if (out) *out = (TaskHandle_t)1;
    if (!strcmp(name, "db_sse")) {
        // Run the event stream task now. The fake send fails after a few
        // events, and vTaskDelay() moves the clock, so the task ends.
        s_in_sse = true;
        s_sse_events = 0;
        fn(arg);
        s_in_sse = false;
    } else if (strstr(name, "restart") || strstr(name, "factory") || strstr(name, "bootother")) {
        fuzz_mutation("reboot task");
    }
    return pdPASS;
}

void vTaskDelay(TickType_t ticks) { fuzz_advance_ms(ticks); }
void vTaskDelete(TaskHandle_t task) { (void)task; }
void xTaskNotifyGive(TaskHandle_t task) { (void)task; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait) { (void)clear; (void)wait; return 0; }

// ---- esp_http_server ---------------------------------------------------------------

static fuzz_route_t s_routes[64];
static unsigned s_route_count;

esp_err_t httpd_register_uri_handler(httpd_handle_t hd, const httpd_uri_t *uri)
{
    FUZZ_CHECK(hd != NULL && uri && uri->uri && uri->handler, "bad route registration");
    FUZZ_CHECK(s_route_count < 64, "too many routes");
    fuzz_route_t *r = &s_routes[s_route_count++];
    r->method = uri->method;
    snprintf(r->uri, sizeof r->uri, "%s", uri->uri);
    r->handler = uri->handler;
    r->user_ctx = uri->user_ctx;
    return ESP_OK;
}

const fuzz_route_t *fuzz_find_route(httpd_method_t method, const char *uri)
{
    for (unsigned i = 0; i < s_route_count; i++)
        if (s_routes[i].method == method && !strcmp(s_routes[i].uri, uri))
            return &s_routes[i];
    return NULL;
}

// The request that is now in a handler.
static httpd_req_t s_req;
static httpd_req_t s_async_req;
static const fuzz_request_t *s_cur;
static fuzz_response_t *s_resp;
static size_t s_consumed;      // body bytes given to the handler
static unsigned s_timeouts;    // consecutive injected timeouts
static unsigned s_stalls;      // timeouts for a stalled client
static bool s_last_short;      // the last recv() gave less than the handler asked for
static int64_t s_held_us;      // time that the httpd task waited for this client

bool fuzz_in_request(void) { return s_cur != NULL; }

size_t fuzz_body_consumed(void) { return s_consumed; }

// The httpd task waits wait_us for the client. No request can hold the task
// longer than FUZZ_REQUEST_BOUND_US.
static void client_wait(int64_t wait_us)
{
    fuzz_advance_ms((uint32_t)(wait_us / 1000));
    s_held_us += wait_us;
    FUZZ_CHECK(s_held_us <= FUZZ_REQUEST_BOUND_US,
               "a slow client held the httpd task longer than the bound");
}

static void check_req(httpd_req_t *r)
{
    FUZZ_CHECK(s_cur != NULL, "httpd call outside a handler");
    FUZZ_CHECK(r == &s_req || r == &s_async_req, "httpd call with an unknown request");
}

// The fake client: one recv() of at most buf_len body bytes.
static int client_recv(char *buf, size_t buf_len)
{
    size_t remaining = s_cur->content_len - s_consumed;
    if (buf_len > remaining) buf_len = remaining;
    if (buf_len == 0) return 0;

    if (s_knobs.client_drips) {
        // One byte just before each timeout, up to Content-Length. After the
        // body of the input, the client sends filler bytes.
        client_wait(DRIP_US);
        buf[0] = s_consumed < s_cur->body_len ? (char)s_cur->body[s_consumed] : 'x';
        s_consumed++;
        s_last_short = buf_len > 1;
        return 1;
    }

    if (s_knobs.recv_timeouts && s_timeouts < 2 && rng_next() % 3 == 0) {
        s_timeouts++;
        client_wait(RECV_WAIT_US);
        return HTTPD_SOCK_ERR_TIMEOUT;
    }
    s_timeouts = 0;

    size_t avail = s_consumed < s_cur->body_len ? s_cur->body_len - s_consumed : 0;
    if (avail == 0) {
        // The client sent fewer bytes than Content-Length.
        if (!s_knobs.client_stalls) return HTTPD_SOCK_ERR_FAIL;   // it closed the socket
        // It keeps the socket open: each recv() waits recv_wait_timeout (5 s)
        // and the httpd task can serve no other client in that time.
        s_stalls++;
        FUZZ_CHECK(s_stalls < STALL_LIMIT,
                   "handler waits for a stalled client without a limit");
        client_wait(RECV_WAIT_US);
        return HTTPD_SOCK_ERR_TIMEOUT;
    }
    size_t n = buf_len < avail ? buf_len : avail;
    if (rng_next() & 1) n = 1 + rng_next() % n;   // TCP can split the body anywhere
    memcpy(buf, s_cur->body + s_consumed, n);
    s_consumed += n;
    s_last_short = n < buf_len;
    return (int)n;
}

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    check_req(r);
    if (!buf) return HTTPD_SOCK_ERR_INVALID;
    // The handler says that buf holds buf_len bytes. Touch the last one, so that
    // ASan finds a handler that gives a length larger than its buffer.
    if (buf_len) ((volatile char *)buf)[buf_len - 1] = ((volatile char *)buf)[buf_len - 1];
    return client_recv(buf, buf_len);
}

// httpd_req_delete() of ESP-IDF v5.3 (httpd_parse.c): after a handler returns
// ESP_OK, read and discard the rest of the body. Stop at the first timeout,
// error or close. There is no time limit and no limit on Content-Length.
static void purge_body(void)
{
    char dummy[PURGE_BUF_LEN];
    bool last_short = s_last_short;   // the handler did not see these reads
    while (s_consumed < s_cur->content_len) {
        if (client_recv(dummy, sizeof dummy) <= 0) break;
    }
    s_last_short = last_short;
}

int httpd_req_to_sockfd(httpd_req_t *r)
{
    check_req(r);
    return -1;
}

static size_t fuzz_strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

// The next two functions copy the logic of ESP-IDF v5.3 httpd_parse.c.
size_t httpd_req_get_url_query_len(httpd_req_t *r)
{
    check_req(r);
    return s_cur->has_query ? strlen(s_cur->query) : 0;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len)
{
    check_req(r);
    if (!buf) return ESP_ERR_INVALID_ARG;
    if (!s_cur->has_query) return ESP_ERR_NOT_FOUND;
    size_t min_buf_len = strlen(s_cur->query) + 1;
    fuzz_strlcpy(buf, s_cur->query, buf_len < min_buf_len ? buf_len : min_buf_len);
    if (buf_len < min_buf_len) return ESP_ERR_HTTPD_RESULT_TRUNC;
    return ESP_OK;
}

esp_err_t httpd_query_key_value(const char *qry_str, const char *key, char *val, size_t val_size)
{
    if (qry_str == NULL || key == NULL || val == NULL) return ESP_ERR_INVALID_ARG;
    const char *qry_ptr = qry_str;
    const size_t buf_len = val_size;
    while (strlen(qry_ptr)) {
        const char *val_ptr = strchr(qry_ptr, '=');
        if (!val_ptr) break;
        size_t offset = (size_t)(val_ptr - qry_ptr);
        if ((offset != strlen(key)) || (strncasecmp(qry_ptr, key, offset))) {
            qry_ptr = strchr(val_ptr, '&');
            if (!qry_ptr) break;
            qry_ptr++;
            continue;
        }
        qry_ptr = strchr(++val_ptr, '&');
        if (!qry_ptr) qry_ptr = val_ptr + strlen(val_ptr);
        val_size = (size_t)(qry_ptr - val_ptr) + 1;
        fuzz_strlcpy(val, val_ptr, val_size < buf_len ? val_size : buf_len);
        if (buf_len < val_size) return ESP_ERR_HTTPD_RESULT_TRUNC;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field)
{
    check_req(r);
    if (strcasecmp(field, "X-DragonBreath-Auth") || !s_cur->has_auth) return 0;
    return strlen(s_cur->auth);
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size)
{
    check_req(r);
    if (strcasecmp(field, "X-DragonBreath-Auth") || !s_cur->has_auth) return ESP_ERR_NOT_FOUND;
    size_t len = strlen(s_cur->auth);
    fuzz_strlcpy(val, s_cur->auth, val_size < len + 1 ? val_size : len + 1);
    if (val_size < len + 1) return ESP_ERR_HTTPD_RESULT_TRUNC;
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    check_req(r);
    FUZZ_CHECK(status && atoi(status) >= 100 && atoi(status) <= 599, "bad HTTP status");
    s_resp->status = atoi(status);
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{
    check_req(r);
    s_resp->json = !strcmp(type, "application/json");
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    check_req(r);
    FUZZ_CHECK(field && value, "NULL response header");
    return ESP_OK;
}

static void resp_append(const char *buf, size_t len)
{
    char *b = realloc(s_resp->body, s_resp->body_len + len + 1);
    FUZZ_CHECK(b != NULL, "out of memory");
    memcpy(b + s_resp->body_len, buf, len);
    s_resp->body = b;
    s_resp->body_len += len;
    b[s_resp->body_len] = '\0';
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len)
{
    check_req(r);
    if (buf_len == HTTPD_RESP_USE_STRLEN) buf_len = buf ? (ssize_t)strlen(buf) : 0;
    FUZZ_CHECK(buf_len >= 0, "negative response length");
    FUZZ_CHECK(buf || buf_len == 0, "NULL response body");
    s_resp->sends++;
    if (buf_len) resp_append(buf, (size_t)buf_len);   // reads every byte: ASan checks the length
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t buf_len)
{
    check_req(r);
    if (buf_len == HTTPD_RESP_USE_STRLEN) buf_len = buf ? (ssize_t)strlen(buf) : 0;
    FUZZ_CHECK(buf_len >= 0, "negative chunk length");
    s_resp->chunks++;
    if (buf_len) resp_append(buf, (size_t)buf_len);
    // An event stream client goes away after a few events.
    if (s_in_sse && ++s_sse_events > 8) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    check_req(r);
    static const int codes[] = { 500, 501, 505, 400, 401, 403, 404, 405, 408, 411, 414, 431 };
    FUZZ_CHECK((unsigned)error < sizeof codes / sizeof codes[0], "bad error code");
    s_resp->status = codes[error];
    s_resp->json = false;
    return httpd_resp_send(r, msg, HTTPD_RESP_USE_STRLEN);
}

esp_err_t httpd_req_async_handler_begin(httpd_req_t *r, httpd_req_t **out)
{
    check_req(r);
    memcpy(&s_async_req, r, sizeof s_async_req);
    *out = &s_async_req;
    return ESP_OK;
}

esp_err_t httpd_req_async_handler_complete(httpd_req_t *r)
{
    FUZZ_CHECK(r == &s_async_req, "complete of an unknown async request");
    return ESP_OK;
}

// ---- input format ----------------------------------------------------------------------

static const char SEP[] = "\n##\n";

static const uint8_t *find_sep(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i++)
        if (!memcmp(p + i, SEP, 4)) return p + i;
    return NULL;
}

// Copy one line (up to '\n') without the line end. Stop at a NUL, as C does.
static size_t take_line(const uint8_t **p, const uint8_t *end, char *out, size_t out_sz)
{
    const uint8_t *s = *p;
    while (*p < end && **p != '\n') (*p)++;
    size_t n = (size_t)(*p - s);
    if (*p < end) (*p)++;   // skip '\n'
    if (n && s[n - 1] == '\r') n--;
    size_t c = n < out_sz - 1 ? n : out_sz - 1;
    memcpy(out, s, c);
    out[c] = '\0';
    return n;
}

bool fuzz_next_request(const uint8_t **data, size_t *size, fuzz_request_t *req)
{
    if (*size == 0) return false;
    const uint8_t *start = *data;
    const uint8_t *sep = find_sep(start, *size);
    const uint8_t *end = sep ? sep : start + *size;
    if (sep) { *size -= (size_t)(sep + 4 - start); *data = sep + 4; }
    else { *data += *size; *size = 0; }

    memset(req, 0, sizeof *req);
    const uint8_t *p = start;
    char line[MAX_URI_LEN + 16];
    size_t n = take_line(&p, end, line, sizeof line);
    if (n >= sizeof line) { req->uri[0] = '\0'; return true; }   // 414: no handler runs

    char *sp = strchr(line, ' ');
    if (!sp) { req->uri[0] = '\0'; return true; }
    *sp = '\0';
    if (!strcmp(line, "WAIT")) {
        req->wait_s = (uint32_t)strtoul(sp + 1, NULL, 10) % 86400;
        req->uri[0] = '\0';
        return true;
    }
    if (!strcmp(line, "GET")) req->method = HTTP_GET;
    else if (!strcmp(line, "POST")) req->method = HTTP_POST;
    else { req->uri[0] = '\0'; return true; }
    char *uri = sp + 1;
    char *q = strchr(uri, '?');
    if (q) {
        *q = '\0';
        req->has_query = true;
        snprintf(req->query, sizeof req->query, "%s", q + 1);
    }
    snprintf(req->uri, sizeof req->uri, "%s", uri);

    // Header lines up to an empty line.
    while (p < end) {
        n = take_line(&p, end, line, sizeof line);
        if (n == 0) break;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        const char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (!strcasecmp(line, "X-DragonBreath-Auth")) {
            req->has_auth = true;
            snprintf(req->auth, sizeof req->auth, "%s", v);
        } else if (!strcasecmp(line, "Content-Length")) {
            req->has_content_len = true;
            // A size_t on the ESP32-C3 has 32 bits.
            unsigned long long n = strtoull(v, NULL, 10);
            req->content_len = n > UINT32_MAX ? UINT32_MAX : (size_t)n;
        }
    }
    req->body = p;
    req->body_len = (size_t)(end - p);
    if (!req->has_content_len) req->content_len = req->body_len;
    return true;
}

// ---- running a handler --------------------------------------------------------------------

void fuzz_host_reset(const fuzz_knobs_t *knobs)
{
    s_knobs = *knobs;
    s_rng = 0x9e3779b9u ^ knobs->seed;
    s_mutations = 0;
    s_now_us = 1000000;
    memset(s_nvs, 0, sizeof s_nvs);
    memset(s_open_ns, 0, sizeof s_open_ns);
    s_route_count = 0;
}

void fuzz_run(const fuzz_route_t *route, const fuzz_request_t *req, fuzz_response_t *resp)
{
    memset(resp, 0, sizeof *resp);
    resp->status = 200;
    memset(&s_req, 0, sizeof s_req);
    s_req.handle = (httpd_handle_t)1;
    s_req.method = (int)req->method;
    s_req.content_len = req->content_len;
    s_req.user_ctx = route->user_ctx;
    snprintf((char *)s_req.uri, sizeof s_req.uri, "%s%s%s",
             req->uri, req->has_query ? "?" : "", req->has_query ? req->query : "");
    s_cur = req;
    s_resp = resp;
    s_consumed = 0;
    s_timeouts = 0;
    s_stalls = 0;
    s_last_short = false;
    s_held_us = 0;
    unsigned before = s_mutations;

    if (route->handler(&s_req) == ESP_OK) purge_body();
    // Else httpd closes the socket.

    s_cur = NULL;
    s_resp = NULL;
    // FUZZ_TRACE=1 prints each request and response, to check a seed by hand.
    if (getenv("FUZZ_TRACE"))
        fprintf(stderr, "%s %s -> %d (%u changes) %.300s\n",
                req->method == HTTP_GET ? "GET" : "POST", req->uri, resp->status,
                s_mutations - before, resp->body ? resp->body : "");
    for (int i = 0; i < 8; i++)
        FUZZ_CHECK(!s_open_ns[i][0], "NVS handle left open after the request");
    FUZZ_CHECK(resp->sends + (resp->chunks ? 1u : 0u) == 1,
               "handler did not send exactly one response");
    if (resp->json && resp->sends == 1) {
        cJSON *j = cJSON_ParseWithLength(resp->body ? resp->body : "", resp->body_len);
        FUZZ_CHECK(j != NULL, "JSON response is not valid JSON");
        cJSON_Delete(j);
    }
    if (resp->status >= 400 && resp->status < 500)
        FUZZ_CHECK(s_mutations == before, "state changed, but the response is 4xx");
    // TCP can deliver a body in parts. A handler that acts on the first part
    // only can apply a value that the client did not send ("max=6" of "max=60").
    if (resp->status < 300)
        FUZZ_CHECK(!s_last_short, "handler used a short recv() as the whole body");
}

void fuzz_response_free(fuzz_response_t *resp)
{
    free(resp->body);
    resp->body = NULL;
}
