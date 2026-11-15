// Host test for pb_httpd_read_body(): a body that arrives in parts, timeouts,
// and a client that stops sending.
#include "pb_httpd_body.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

// Each step of a fake client: > 0 = send that many body bytes, or an error code.
static const int *s_script;
static size_t s_steps, s_step;
static const char *s_body;
static size_t s_sent;
static unsigned s_calls;

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    s_calls++;
    CHECK(buf_len > 0);
    CHECK(s_sent + buf_len <= r->content_len);   // never asks past Content-Length
    if (s_step >= s_steps) return HTTPD_SOCK_ERR_TIMEOUT;   // the client stalls
    int n = s_script[s_step++];
    if (n <= 0) return n;
    if ((size_t)n > buf_len) n = (int)buf_len;
    memcpy(buf, s_body + s_sent, (size_t)n);
    s_sent += (size_t)n;
    return n;
}

static bool run(const char *body, const int *script, size_t steps, char *out)
{
    httpd_req_t req = { .content_len = strlen(body) };
    s_script = script;
    s_steps = steps;
    s_step = 0;
    s_body = body;
    s_sent = 0;
    s_calls = 0;
    memset(out, 0, 64);
    return pb_httpd_read_body(&req, out, req.content_len);
}

int main(void)
{
    char out[64];
    const char *body = "max=60&comms_ms=30000";

    // One read.
    static const int one[] = { 100 };
    CHECK(run(body, one, 1, out) && strcmp(out, body) == 0);

    // A body in parts: one recv() gives only "max=6". The helper reads on.
    static const int parts[] = { 5, 1, 3, 100 };
    CHECK(run(body, parts, 4, out) && strcmp(out, body) == 0);

    // Timeouts below the limit between the parts.
    static const int slow[] = { HTTPD_SOCK_ERR_TIMEOUT, 5, HTTPD_SOCK_ERR_TIMEOUT, 100 };
    CHECK(run(body, slow, 4, out) && strcmp(out, body) == 0);

    // A client that sends a part and then stops: the read gives up after
    // PB_HTTPD_BODY_TIMEOUTS_MAX timeouts and does not wait forever.
    static const int stall[] = { 5 };
    CHECK(!run(body, stall, 1, out));
    CHECK(s_calls == 1 + PB_HTTPD_BODY_TIMEOUTS_MAX);

    // A client that closes the connection, or a socket error.
    static const int closed[] = { 5, 0 };
    CHECK(!run(body, closed, 2, out));
    static const int fail[] = { HTTPD_SOCK_ERR_FAIL };
    CHECK(!run(body, fail, 1, out));

    puts("pb_httpd_body host test: OK");
    return 0;
}
