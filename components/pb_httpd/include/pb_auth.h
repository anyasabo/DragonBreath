// SPDX-License-Identifier: MIT
// pb_auth — pure decisions for the X-DragonBreath-Auth gate (no ESP-IDF deps,
// host-testable). pb_httpd collects the inputs (header, NVS token, Wi-Fi link,
// time) and applies the result.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Maximum control-token length in characters (the NVS value and the header).
#define PB_AUTH_TOKEN_MAX 64

typedef enum {
    PB_AUTH_OK = 0,
    PB_AUTH_MISSING,         // no usable header (absent, empty or too long)
    PB_AUTH_MISMATCH,        // a token is set and the header does not equal it
    PB_AUTH_TOKEN_REQUIRED,  // the build requires a token, none is set, LAN-reachable
    PB_AUTH_LOCKED,          // too many recent mismatches: all checks fail for a time
} pb_auth_result_t;

typedef struct {
    bool header_present;  // a non-empty header that fits PB_AUTH_TOKEN_MAX
    bool token_set;       // a control token is stored in NVS
    bool token_match;     // header equals the stored token (pb_auth_token_equal)
    bool require_token;   // CONFIG_DB_REQUIRE_CONTROL_TOKEN
    bool lan_reachable;   // the STA interface is associated (LAN clients can reach us)
    bool on_setup_ap;     // the request arrived on the setup AP interface
    bool locked_out;      // pb_auth_lockout_active() at the time of the request
} pb_auth_input_t;

// Order: lockout, then header presence, then the token.
// With no token set, the header is a presence-only CSRF gate (today's behavior).
// If require_token is set, that gate applies only to a request that arrived on
// the setup AP while the station is not associated.
pb_auth_result_t pb_auth_decide(const pb_auth_input_t *in);

// True if token is 1..PB_AUTH_TOKEN_MAX characters, each printable ASCII
// without space (0x21-0x7E). Use it before a token is stored.
bool pb_auth_token_valid(const char *token);

// True if a socket's local address equals the IPv4 address ap_ip (4 bytes in
// network order, as in sin_addr). addr is the 4-byte sin_addr (is_v6 false) or
// the 16-byte sin6_addr (is_v6 true). An IPv6 address matches only in the
// IPv4-mapped form ::ffff:a.b.c.d (dual-stack httpd socket). An ap_ip of
// 0.0.0.0 never matches.
bool pb_auth_local_addr_is(bool is_v6, const uint8_t *addr, const uint8_t ap_ip[4]);

// Compare two tokens in time that does not depend on where they differ.
// The loop always runs PB_AUTH_TOKEN_MAX steps. A length above
// PB_AUTH_TOKEN_MAX never matches.
bool pb_auth_token_equal(const char *given, size_t given_len,
                         const char *token, size_t token_len);

// Failed-attempt limiter (global, RAM-only). After max_fails mismatches inside
// window_ms, every auth check fails for lockout_ms. Times are a free-running
// millisecond counter. Unsigned arithmetic handles the wrap.
typedef struct {
    uint32_t max_fails;
    uint32_t window_ms;
    uint32_t lockout_ms;
} pb_auth_lockout_cfg_t;

typedef struct {
    uint32_t window_start_ms;
    uint32_t fails;
    uint32_t lock_start_ms;
    bool locked;
} pb_auth_lockout_t;

// True while a lockout is active. Ends an expired lockout (and resets the count).
bool pb_auth_lockout_active(pb_auth_lockout_t *s, const pb_auth_lockout_cfg_t *cfg,
                            uint32_t now_ms);

// Record one mismatch. Returns true only for the failure that starts a lockout,
// so the caller can log once per lockout.
bool pb_auth_lockout_fail(pb_auth_lockout_t *s, const pb_auth_lockout_cfg_t *cfg,
                          uint32_t now_ms);
