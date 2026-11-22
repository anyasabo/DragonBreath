// pb_token — compare a control token in constant time.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// The longest control token that the API accepts.
#define PB_TOKEN_MAX 64

// True if a and b are the same string of at most PB_TOKEN_MAX characters. The loop
// always runs PB_TOKEN_MAX + 1 times and has no early exit on the first different
// character, so the time does not tell how much of a guess is correct. A string
// longer than PB_TOKEN_MAX is never equal. The function reads no byte after the
// terminating NUL of either string.
static inline bool pb_token_equal(const char *a, const char *b)
{
    unsigned diff = 0;
    bool a_end = false, b_end = false;
    for (size_t i = 0; i <= PB_TOKEN_MAX; i++) {
        unsigned char ca = a_end ? 0 : (unsigned char)a[i];
        unsigned char cb = b_end ? 0 : (unsigned char)b[i];
        diff |= (unsigned)(ca ^ cb);
        if (ca == 0) a_end = true;
        if (cb == 0) b_end = true;
    }
    return diff == 0 && a_end && b_end;
}
