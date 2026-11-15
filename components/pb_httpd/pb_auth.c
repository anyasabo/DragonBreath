// SPDX-License-Identifier: MIT
#include "pb_auth.h"

#include <string.h>

pb_auth_result_t pb_auth_decide(const pb_auth_input_t *in)
{
    if (in->locked_out) return PB_AUTH_LOCKED;
    if (!in->header_present) return PB_AUTH_MISSING;
    if (in->token_set) return in->token_match ? PB_AUTH_OK : PB_AUTH_MISMATCH;
    // No token: presence-only CSRF gate. A hardened build accepts that only for a
    // setup AP client while the LAN cannot reach the device.
    if (in->require_token && (in->lan_reachable || !in->on_setup_ap))
        return PB_AUTH_TOKEN_REQUIRED;
    return PB_AUTH_OK;
}

bool pb_auth_token_valid(const char *token)
{
    if (!token || !token[0]) return false;
    size_t n = 0;
    for (; token[n]; n++) {
        if (n >= PB_AUTH_TOKEN_MAX) return false;
        unsigned char ch = (unsigned char)token[n];
        if (ch < 0x21 || ch > 0x7E) return false;
    }
    return true;
}

bool pb_auth_local_addr_is(bool is_v6, const uint8_t *addr, const uint8_t ap_ip[4])
{
    static const uint8_t zero[4] = {0, 0, 0, 0};
    // lwIP has no IN6_IS_ADDR_V4MAPPED: check the ::ffff:0:0/96 prefix here.
    static const uint8_t v4mapped[12] = {0,0,0,0, 0,0,0,0, 0,0,0xff,0xff};
    if (!addr || !ap_ip || memcmp(ap_ip, zero, 4) == 0) return false;
    if (is_v6) {
        if (memcmp(addr, v4mapped, sizeof v4mapped) != 0) return false;
        addr += sizeof v4mapped;
    }
    return memcmp(addr, ap_ip, 4) == 0;
}

bool pb_auth_token_equal(const char *given, size_t given_len,
                         const char *token, size_t token_len)
{
    if (given_len > PB_AUTH_TOKEN_MAX || token_len > PB_AUTH_TOKEN_MAX) return false;
    // Fixed loop count. Bytes past each length compare as 0. The length
    // difference goes into the same accumulator, so "ab" never equals "ab\0".
    unsigned diff = (unsigned)(given_len ^ token_len);
    for (size_t i = 0; i < PB_AUTH_TOKEN_MAX; i++) {
        unsigned char a = i < given_len ? (unsigned char)given[i] : 0;
        unsigned char b = i < token_len ? (unsigned char)token[i] : 0;
        diff |= (unsigned)(a ^ b);
    }
    return diff == 0;
}

bool pb_auth_lockout_active(pb_auth_lockout_t *s, const pb_auth_lockout_cfg_t *cfg,
                            uint32_t now_ms)
{
    if (!s->locked) return false;
    if ((uint32_t)(now_ms - s->lock_start_ms) < cfg->lockout_ms) return true;
    s->locked = false;
    s->fails = 0;
    return false;
}

bool pb_auth_lockout_fail(pb_auth_lockout_t *s, const pb_auth_lockout_cfg_t *cfg,
                          uint32_t now_ms)
{
    if (pb_auth_lockout_active(s, cfg, now_ms)) return false;   // already locked
    if (s->fails == 0 || (uint32_t)(now_ms - s->window_start_ms) >= cfg->window_ms) {
        s->window_start_ms = now_ms;
        s->fails = 0;
    }
    s->fails++;
    if (s->fails < cfg->max_fails) return false;
    s->locked = true;
    s->lock_start_ms = now_ms;
    s->fails = 0;
    return true;
}
