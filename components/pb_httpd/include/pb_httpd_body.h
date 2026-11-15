// SPDX-License-Identifier: MIT
#pragma once
// Bounded read of a request body. httpd_req_recv() can return a part of the
// body, because TCP can deliver it in parts. It returns HTTPD_SOCK_ERR_TIMEOUT
// after recv_wait_timeout (3 s in db_portal_start()) with no data. The httpd task serves one request
// at a time. Thus a body read must not wait for a stalled or slow client
// without a limit.
//
// This is a copy of dc_portal_read_body() in dragon-core dc_portal_body.h, with
// the same limits and the same tests. DragonBreath pins the upstream
// dragon-core, so it keeps its own copy.
//
// This header has no ESP-IDF dependency, so a host test can drive it with a
// fake receive function and a fake clock. pb_httpd_read_body() in pb_httpd.c
// connects it to httpd_req_recv() and esp_timer_get_time().

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The value that the receive function returns for "no data before the socket
// timeout". It is the same as HTTPD_SOCK_ERR_TIMEOUT.
#define PB_HTTPD_BODY_RECV_TIMEOUT (-3)

typedef struct {
    // Most consecutive timeouts with no data. Data resets the count.
    int idle_timeouts_max;
    // Most time for the whole body, in microseconds. 0 = no limit.
    int64_t deadline_us;
} pb_httpd_body_limits_t;

typedef int (*pb_httpd_recv_fn)(void *ctx, char *buf, size_t len);
typedef int64_t (*pb_httpd_now_fn)(void);

// Bodies are small (API_BODY_MAX, 1536 bytes, or less). Stop after 3 idle
// timeouts in a row, or after 10 s in total. The read checks the deadline
// before each receive, and one receive can wait for one recv_wait_timeout
// (3 s). Thus a client that sends one byte for each timeout holds the httpd
// task for about 13 s at most.
#define PB_HTTPD_BODY_LIMITS ((pb_httpd_body_limits_t){ .idle_timeouts_max = 3, .deadline_us = 10 * 1000000LL })

// Read exactly len bytes into buf. Return true if all bytes arrived. Return
// false if the client closed the socket, gave an error, sent no data for
// idle_timeouts_max timeouts in a row, or passed the deadline.
static inline bool pb_httpd_read_body_bounded(pb_httpd_recv_fn recv, pb_httpd_now_fn now, void *ctx,
                                              char *buf, size_t len, pb_httpd_body_limits_t lim)
{
    const int64_t start = now ? now() : 0;
    size_t got = 0;
    int idle = 0;
    while (got < len) {
        if (lim.deadline_us > 0 && now && now() - start > lim.deadline_us) return false;
        int r = recv(ctx, buf + got, len - got);
        if (r == PB_HTTPD_BODY_RECV_TIMEOUT) {
            if (++idle >= lim.idle_timeouts_max) return false;
            continue;
        }
        if (r <= 0) return false;
        idle = 0;
        got += (size_t)r;
    }
    return true;
}

// True if the handler did not read all of the body. If a handler returns ESP_OK,
// httpd_req_delete() reads and discards the rest of the body with no time limit,
// and ESP-IDF does not limit Content-Length. Thus the handler must return
// ESP_FAIL: then httpd closes the socket.
static inline bool pb_httpd_body_unread(size_t content_len, size_t body_read)
{
    return body_read < content_len;
}
