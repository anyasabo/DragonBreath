// Host test of pb_token_equal(): the constant-time control-token compare.
#include <stdio.h>
#include <string.h>
#include "pb_token.h"

static int failures;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

int main(void)
{
    char long_a[PB_TOKEN_MAX + 2], long_b[PB_TOKEN_MAX + 2];
    memset(long_a, 'x', sizeof long_a - 1); long_a[sizeof long_a - 1] = '\0';
    memcpy(long_b, long_a, sizeof long_b);

    CHECK(pb_token_equal("secret-token", "secret-token"));
    CHECK(!pb_token_equal("secret-token", "secret-tokeN"));
    CHECK(!pb_token_equal("secret", "secret-token"));      // prefix
    CHECK(!pb_token_equal("secret-token", "secret"));      // longer
    CHECK(!pb_token_equal("", "a"));
    CHECK(pb_token_equal("", ""));
    // Longer than PB_TOKEN_MAX: never equal, also to itself.
    CHECK(!pb_token_equal(long_a, long_b));
    char max_a[PB_TOKEN_MAX + 1], max_b[PB_TOKEN_MAX + 1];
    memset(max_a, 'y', PB_TOKEN_MAX); max_a[PB_TOKEN_MAX] = '\0';
    memcpy(max_b, max_a, sizeof max_b);
    CHECK(pb_token_equal(max_a, max_b));                  // exactly PB_TOKEN_MAX
    max_b[PB_TOKEN_MAX - 1] = 'z';
    CHECK(!pb_token_equal(max_a, max_b));

    if (failures) return 1;
    printf("pb_token host test: PASS\n");
    return 0;
}
