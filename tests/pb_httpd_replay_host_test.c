// SPDX-License-Identifier: MIT
// Host test for the replay cache of POST /api/v2/command (pb_httpd_replay.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pb_httpd_replay.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int frees;
static void count_free(void *p) { frees++; free(p); }

static char *dup(const char *s)
{
    char *p = malloc(strlen(s) + 1);
    if (!p) abort();
    strcpy(p, s);
    return p;
}

static void test_empty_cache_finds_nothing(void)
{
    pb_replay_t c = { .free_fn = count_free };
    CHECK(pb_replay_find(&c, "a", "r1") == NULL);
    CHECK(pb_replay_find(&c, "", "") == NULL);
    CHECK(pb_replay_find(NULL, "a", "r1") == NULL);
    CHECK(pb_replay_find(&c, NULL, "r1") == NULL);
}

static void test_store_find_and_evict(void)
{
    pb_replay_t c = { .free_fn = count_free };
    frees = 0;
    pb_replay_store(&c, "a", "r1", 10, dup("{\"one\":1}"));
    pb_replay_store(&c, "b", "r1", 11, dup("{\"two\":2}"));
    const pb_replay_entry_t *e = pb_replay_find(&c, "a", "r1");
    CHECK(e && e->revision == 10 && strcmp(e->response, "{\"one\":1}") == 0);
    e = pb_replay_find(&c, "b", "r1");                // same request id, other actor
    CHECK(e && e->revision == 11);
    CHECK(pb_replay_find(&c, "a", "r2") == NULL);
    CHECK(frees == 0);

    // A third response replaces the oldest one and frees it.
    pb_replay_store(&c, "a", "r3", 12, dup("{\"three\":3}"));
    CHECK(frees == 1);
    CHECK(pb_replay_find(&c, "a", "r1") == NULL);
    CHECK(pb_replay_find(&c, "b", "r1") != NULL);
    CHECK(pb_replay_find(&c, "a", "r3") != NULL);

    // A large response is kept whole (the old static cache cut it at 3 KB).
    char *big = malloc(8000);
    if (!big) abort();
    memset(big, 'x', 7999);
    big[7999] = '\0';
    pb_replay_store(&c, "c", "r4", 13, big);
    e = pb_replay_find(&c, "c", "r4");
    CHECK(e && strlen(e->response) == 7999);

    for (int i = 0; i < PB_REPLAY_ENTRIES; i++) {    // release all for the leak check
        free(c.e[i].response);
        c.e[i].response = NULL;
    }
}

static void test_bad_ids_free_the_response(void)
{
    pb_replay_t c = { .free_fn = count_free };
    char long_actor[PB_REPLAY_ACTOR_LEN + 2];
    char long_req[PB_REPLAY_REQUEST_LEN + 2];
    memset(long_actor, 'a', sizeof long_actor - 1);
    long_actor[sizeof long_actor - 1] = '\0';
    memset(long_req, 'r', sizeof long_req - 1);
    long_req[sizeof long_req - 1] = '\0';
    frees = 0;
    pb_replay_store(&c, long_actor, "r", 1, dup("x"));
    pb_replay_store(&c, "a", long_req, 1, dup("x"));
    pb_replay_store(&c, NULL, "r", 1, dup("x"));
    pb_replay_store(&c, "a", NULL, 1, dup("x"));
    pb_replay_store(&c, "a", "r", 1, NULL);           // nothing to free
    pb_replay_store(NULL, "a", "r", 1, NULL);
    CHECK(frees == 4);
    CHECK(c.next == 0);

    // Ids at the maximum length are kept.
    long_actor[PB_REPLAY_ACTOR_LEN] = '\0';
    long_req[PB_REPLAY_REQUEST_LEN] = '\0';
    pb_replay_store(&c, long_actor, long_req, 5, dup("ok"));
    const pb_replay_entry_t *e = pb_replay_find(&c, long_actor, long_req);
    CHECK(e && e->revision == 5);
    free(c.e[0].response);
}

int main(void)
{
    test_empty_cache_finds_nothing();
    test_store_find_and_evict();
    test_bad_ids_free_the_response();
    if (failures) {
        fprintf(stderr, "pb_httpd_replay: %d failure(s)\n", failures);
        return 1;
    }
    printf("pb_httpd_replay host test: OK\n");
    return 0;
}
