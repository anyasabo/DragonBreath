// Shared host fakes for the HTTP handler fuzz harnesses: a fake esp_http_server
// request, an in-memory NVS, and the invariant checks that apply to every
// request. See fuzz_httpd.c and fuzz_portal.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"

#define FUZZ_CHECK(cond, what) \
    do { if (!(cond)) fuzz_fail(__FILE__, __LINE__, (what)); } while (0)

__attribute__((noreturn)) void fuzz_fail(const char *file, int line, const char *what);

// Global knobs for one fuzz input. They come from the first bytes of the input.
typedef struct {
    uint8_t seed;          // drives the recv() chunk sizes and the injected timeouts
    bool nvs_write_fails;  // each NVS write or commit returns an error
    bool recv_timeouts;    // httpd_req_recv() returns a few timeouts between chunks
    bool client_stalls;    // a body shorter than Content-Length: the client keeps the socket open
    bool client_drips;     // the client sends 1 byte for each 2.9 s, up to Content-Length
    uint8_t device[2];     // device state; each harness gives its own meaning
} fuzz_knobs_t;

// One HTTP request, cut out of the fuzz input.
typedef struct {
    uint32_t wait_s;           // "WAIT <s>": no request, the time moves forward
    httpd_method_t method;
    char uri[160];             // path only
    char query[600];           // the URL query, without '?'
    bool has_query;
    bool has_auth;
    char auth[200];            // value of the X-DragonBreath-Auth header
    bool has_content_len;
    size_t content_len;        // Content-Length header, at most UINT32_MAX (else the body length)
    const uint8_t *body;
    size_t body_len;
} fuzz_request_t;

typedef struct {
    int status;                // 200 if the handler did not set a status
    unsigned sends;            // complete responses (send, sendstr, send_err)
    unsigned chunks;           // chunked sends
    bool json;                 // Content-Type was application/json
    char *body;                // the response body, NUL-terminated
    size_t body_len;
} fuzz_response_t;

// Reset the NVS store, the routes and the request state for a new input.
void fuzz_host_reset(const fuzz_knobs_t *knobs);
// Read the knobs from the first 4 bytes of the input. Return the number of bytes used.
size_t fuzz_read_knobs(const uint8_t *data, size_t size, fuzz_knobs_t *knobs);

// Cut the next request out of the input. The format is like HTTP/1.1:
//   METHOD /path[?query]\n
//   Header: value\n        (X-DragonBreath-Auth and Content-Length only)
//   \n
//   body
// Requests are separated by "\n##\n". A "WAIT <seconds>" line in place of a
// request moves the clock. Return false at the end of the input.
bool fuzz_next_request(const uint8_t **data, size_t *size, fuzz_request_t *req);

// A route that the code under test registered with httpd_register_uri_handler().
typedef struct {
    httpd_method_t method;
    char uri[64];
    esp_err_t (*handler)(httpd_req_t *);
    void *user_ctx;
} fuzz_route_t;

// Find a route. Return NULL if there is none.
const fuzz_route_t *fuzz_find_route(httpd_method_t method, const char *uri);

// Check that the code under test registered exactly these routes, for example
// "GET /api/v2/info". A missing route makes a fuzzer that tests nothing.
void fuzz_check_routes(const char *const *routes, size_t count);

// The most time that one body read can hold the httpd task: the time that the
// task waits for the client in recv() for the body. The fakes do not model the
// header read or slow sends, so this bound does not cover them.
// A body read (pb_httpd and dc_portal) checks its 10 s deadline before each
// recv(), and one recv() waits at most 3 s (db_portal_start()). Thus a body
// read stops after about 13 s. The bound has 2 s of margin.
#define FUZZ_REQUEST_BOUND_US (15 * 1000000LL)

// Run one route on one request, as esp_http_server does, and check the general
// invariants:
// - the handler sends exactly one response (or a chunked/async response),
// - a JSON response body is valid JSON,
// - a 4xx response comes with no state change (fuzz_mutation()),
// - a 2xx response does not come from a body that recv() gave only in part,
// - the handler never asks httpd_req_recv() for more than it can hold,
// - the request holds the httpd task for FUZZ_REQUEST_BOUND_US at most. If the
//   handler returns ESP_OK, httpd_req_delete() reads and discards the rest of
//   the body. This time counts too.
void fuzz_run(const fuzz_route_t *route, const fuzz_request_t *req, fuzz_response_t *resp);
// Body bytes that the client gave to the handler of the current request.
size_t fuzz_body_consumed(void);
void fuzz_response_free(fuzz_response_t *resp);

// The fakes call this for each accepted change of device state or NVS.
void fuzz_mutation(const char *what);
unsigned fuzz_mutation_count(void);
// The request that is now in a handler, or NULL.
bool fuzz_in_request(void);

// Direct access to the NVS store for the harnesses (no invariant checks).
void fuzz_nvs_put_str(const char *key, const char *value);
// Move the fake clock (esp_timer_get_time()) forward.
void fuzz_advance_ms(uint32_t ms);
