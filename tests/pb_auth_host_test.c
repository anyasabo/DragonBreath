// SPDX-License-Identifier: MIT
// Host unit test for the pure auth decisions (no ESP/FreeRTOS deps).
#include "pb_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

static bool eq(const char *a, const char *b)
{
    return pb_auth_token_equal(a, strlen(a), b, strlen(b));
}

static void test_token_equal(void)
{
    CHECK(eq("secret", "secret"));
    CHECK(!eq("secret", "secreT"));
    CHECK(!eq("Secret", "secret"));
    CHECK(!eq("secret", "secret2"));   // prefix
    CHECK(!eq("secret2", "secret"));
    CHECK(!eq("", "secret"));
    CHECK(eq("", ""));

    // An embedded NUL past the given length must not match a shorter token.
    CHECK(!pb_auth_token_equal("ab\0", 3, "ab", 2));

    char max[PB_AUTH_TOKEN_MAX + 2];
    memset(max, 'x', sizeof max);
    max[PB_AUTH_TOKEN_MAX] = '\0';
    CHECK(pb_auth_token_equal(max, PB_AUTH_TOKEN_MAX, max, PB_AUTH_TOKEN_MAX));
    char max2[PB_AUTH_TOKEN_MAX + 1];
    memcpy(max2, max, sizeof max2);
    max2[PB_AUTH_TOKEN_MAX - 1] = 'y';   // differs only in the last byte
    CHECK(!pb_auth_token_equal(max, PB_AUTH_TOKEN_MAX, max2, PB_AUTH_TOKEN_MAX));

    // Over the maximum never matches, also against itself.
    max[PB_AUTH_TOKEN_MAX] = 'x';
    max[PB_AUTH_TOKEN_MAX + 1] = '\0';
    CHECK(!pb_auth_token_equal(max, PB_AUTH_TOKEN_MAX + 1, max, PB_AUTH_TOKEN_MAX + 1));
}

static void test_token_valid(void)
{
    CHECK(pb_auth_token_valid("a"));
    CHECK(pb_auth_token_valid("!~Az09-_.:/+="));
    CHECK(!pb_auth_token_valid(NULL));
    CHECK(!pb_auth_token_valid(""));
    CHECK(!pb_auth_token_valid("has space"));
    CHECK(!pb_auth_token_valid("tab\there"));
    CHECK(!pb_auth_token_valid("new\nline"));
    CHECK(!pb_auth_token_valid("del\x7f"));
    CHECK(!pb_auth_token_valid("caf\xc3\xa9"));   // UTF-8 is not ASCII

    char tok[PB_AUTH_TOKEN_MAX + 2];
    memset(tok, 'x', sizeof tok);
    tok[PB_AUTH_TOKEN_MAX] = '\0';
    CHECK(pb_auth_token_valid(tok));                // exactly the maximum
    tok[PB_AUTH_TOKEN_MAX] = 'x';
    tok[PB_AUTH_TOKEN_MAX + 1] = '\0';
    CHECK(!pb_auth_token_valid(tok));               // one over
}

static pb_auth_input_t base(void)
{
    pb_auth_input_t in = {
        .header_present = true,
        .token_set = false,
        .token_match = false,
        .require_token = false,
        .lan_reachable = true,
        .on_setup_ap = false,
        .locked_out = false,
    };
    return in;
}

static void test_decide_default_build(void)
{
    // Today's behavior: no token -> any header passes, a missing one fails.
    pb_auth_input_t in = base();
    CHECK(pb_auth_decide(&in) == PB_AUTH_OK);
    in.header_present = false;
    CHECK(pb_auth_decide(&in) == PB_AUTH_MISSING);

    // A token is set: exact match only.
    in = base();
    in.token_set = true;
    CHECK(pb_auth_decide(&in) == PB_AUTH_MISMATCH);
    in.token_match = true;
    CHECK(pb_auth_decide(&in) == PB_AUTH_OK);
    in.header_present = false;
    in.token_match = false;
    CHECK(pb_auth_decide(&in) == PB_AUTH_MISSING);
}

static void test_decide_require_token(void)
{
    pb_auth_input_t in = base();
    in.require_token = true;

    // No token, LAN-reachable: refused with the specific reason, on any interface.
    CHECK(pb_auth_decide(&in) == PB_AUTH_TOKEN_REQUIRED);
    in.on_setup_ap = true;
    CHECK(pb_auth_decide(&in) == PB_AUTH_TOKEN_REQUIRED);
    // Station not associated, but the request did not arrive on the setup AP.
    in.lan_reachable = false;
    in.on_setup_ap = false;
    CHECK(pb_auth_decide(&in) == PB_AUTH_TOKEN_REQUIRED);
    // Setup AP client while the station is not associated: presence gate.
    in.on_setup_ap = true;
    CHECK(pb_auth_decide(&in) == PB_AUTH_OK);
    in.header_present = false;
    CHECK(pb_auth_decide(&in) == PB_AUTH_MISSING);

    // With a token, require_token changes nothing, on either interface.
    in = base();
    in.require_token = true;
    in.token_set = true;
    CHECK(pb_auth_decide(&in) == PB_AUTH_MISMATCH);
    in.lan_reachable = false;
    in.on_setup_ap = true;
    CHECK(pb_auth_decide(&in) == PB_AUTH_MISMATCH);
    in.token_match = true;
    CHECK(pb_auth_decide(&in) == PB_AUTH_OK);
}

static void test_decide_lockout_first(void)
{
    // A lockout rejects even the correct token (else it does not stop guessing).
    pb_auth_input_t in = base();
    in.token_set = true;
    in.token_match = true;
    in.locked_out = true;
    CHECK(pb_auth_decide(&in) == PB_AUTH_LOCKED);
    in = base();
    in.locked_out = true;
    CHECK(pb_auth_decide(&in) == PB_AUTH_LOCKED);
}

static const pb_auth_lockout_cfg_t CFG = { .max_fails = 3, .window_ms = 1000, .lockout_ms = 5000 };

static void test_lockout_counts_and_expires(void)
{
    pb_auth_lockout_t s = {0};
    CHECK(!pb_auth_lockout_active(&s, &CFG, 0));
    CHECK(!pb_auth_lockout_fail(&s, &CFG, 100));
    CHECK(!pb_auth_lockout_fail(&s, &CFG, 200));
    CHECK(pb_auth_lockout_fail(&s, &CFG, 300));      // third inside the window: starts
    CHECK(pb_auth_lockout_active(&s, &CFG, 300));
    CHECK(!pb_auth_lockout_fail(&s, &CFG, 400));     // already locked: no second "start"
    CHECK(pb_auth_lockout_active(&s, &CFG, 5299));   // 4999 ms after the start
    CHECK(!pb_auth_lockout_active(&s, &CFG, 5300));  // expired
    CHECK(s.fails == 0);
    // A new count starts after the lockout.
    CHECK(!pb_auth_lockout_fail(&s, &CFG, 5400));
    CHECK(!pb_auth_lockout_fail(&s, &CFG, 5500));
    CHECK(pb_auth_lockout_fail(&s, &CFG, 5600));
}

static void test_lockout_fixed_window(void)
{
    // The window is fixed: it starts at the first failure and does not slide.
    pb_auth_lockout_t s = {0};
    CHECK(!pb_auth_lockout_fail(&s, &CFG, 0));
    CHECK(!pb_auth_lockout_fail(&s, &CFG, 900));
    CHECK(!pb_auth_lockout_fail(&s, &CFG, 1000));    // window ended: count restarts at 1
    CHECK(!pb_auth_lockout_fail(&s, &CFG, 1500));
    CHECK(!pb_auth_lockout_active(&s, &CFG, 1600));
    CHECK(pb_auth_lockout_fail(&s, &CFG, 1999));
}

static void test_lockout_wraps(void)
{
    // The millisecond counter wraps after about 49 days.
    pb_auth_lockout_t s = {0};
    uint32_t t = 0xFFFFFF00u;
    CHECK(!pb_auth_lockout_fail(&s, &CFG, t));
    CHECK(!pb_auth_lockout_fail(&s, &CFG, t + 0x80u));
    CHECK(pb_auth_lockout_fail(&s, &CFG, t + 0x180u));   // wrapped, still inside 1000 ms
    CHECK(pb_auth_lockout_active(&s, &CFG, t + 0x180u + 4999u));
    CHECK(!pb_auth_lockout_active(&s, &CFG, t + 0x180u + 5000u));
}

int main(void)
{
    test_token_equal();
    test_token_valid();
    test_decide_default_build();
    test_decide_require_token();
    test_decide_lockout_first();
    test_lockout_counts_and_expires();
    test_lockout_fixed_window();
    test_lockout_wraps();
    puts("pb_auth host test: OK");
    return 0;
}
