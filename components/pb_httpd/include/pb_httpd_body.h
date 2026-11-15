// SPDX-License-Identifier: MIT
#pragma once
// Read a whole request body. httpd_req_recv() can return a part of the body,
// because TCP can deliver it in parts. It returns HTTPD_SOCK_ERR_TIMEOUT after
// recv_wait_timeout (5 s) with no data. The httpd task serves one request at a
// time, so a body read must not wait for a stalled client without a limit.

#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>

// Most timeouts in one body read: a stalled client holds the httpd task for
// at most this many recv_wait_timeout periods.
#define PB_HTTPD_BODY_TIMEOUTS_MAX 3

// Read exactly len bytes of the body into buf and return true. If the client
// closes the connection, gives a socket error or sends no data in
// PB_HTTPD_BODY_TIMEOUTS_MAX timeouts, return false.
static inline bool pb_httpd_read_body(httpd_req_t *req, char *buf, size_t len)
{
    size_t got = 0;
    int timeouts = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts >= PB_HTTPD_BODY_TIMEOUTS_MAX) return false;
            continue;
        }
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}
