// SPDX-License-Identifier: MIT
// pb_httpd_replay — the replay cache of POST /api/v2/command (see docs/api-v2.md).
//
// The cache keeps the last PB_REPLAY_ENTRIES successful responses, keyed by
// (actor.id, request_id). A response is the heap string from
// cJSON_PrintUnformatted(). The cache owns it and frees it with the free
// function of the cache when a newer response replaces it. Before the first
// command, the cache uses no heap.
//
// Not thread-safe: only the HTTP server task uses it. Pure C, host-tested.
#pragma once

#include <stdint.h>

#define PB_REPLAY_ENTRIES     2
#define PB_REPLAY_ACTOR_LEN   31   // PB_POLICY_OWNER_LEN
#define PB_REPLAY_REQUEST_LEN 64

typedef struct {
    char actor_id[PB_REPLAY_ACTOR_LEN + 1];
    char request_id[PB_REPLAY_REQUEST_LEN + 1];
    uint32_t revision;
    char *response;               // owned; NULL: the entry is empty
} pb_replay_entry_t;

typedef struct {
    pb_replay_entry_t e[PB_REPLAY_ENTRIES];
    unsigned next;
    void (*free_fn)(void *);      // frees a response (cJSON_free on the device)
} pb_replay_t;

// The entry for (actor_id, request_id), or NULL.
const pb_replay_entry_t *pb_replay_find(const pb_replay_t *c, const char *actor_id,
                                        const char *request_id);

// Store a response and take ownership of it. The oldest entry is replaced and
// its response freed. An id that is too long, or a NULL argument: the response
// is freed and nothing is stored.
void pb_replay_store(pb_replay_t *c, const char *actor_id, const char *request_id,
                     uint32_t revision, char *response);
