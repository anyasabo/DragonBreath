// SPDX-License-Identifier: MIT
// pb_httpd_replay — see pb_httpd_replay.h.
#include "pb_httpd_replay.h"

#include <stddef.h>
#include <string.h>

const pb_replay_entry_t *pb_replay_find(const pb_replay_t *c, const char *actor_id,
                                        const char *request_id)
{
    if (!c || !actor_id || !request_id) return NULL;
    for (size_t i = 0; i < PB_REPLAY_ENTRIES; i++) {
        const pb_replay_entry_t *e = &c->e[i];
        if (e->response && e->request_id[0]
                && strcmp(e->actor_id, actor_id) == 0
                && strcmp(e->request_id, request_id) == 0)
            return e;
    }
    return NULL;
}

static void release(pb_replay_t *c, char *p)
{
    if (p && c->free_fn) c->free_fn(p);
}

void pb_replay_store(pb_replay_t *c, const char *actor_id, const char *request_id,
                     uint32_t revision, char *response)
{
    if (!c) return;
    if (!actor_id || !request_id || !response
            || strlen(actor_id) > PB_REPLAY_ACTOR_LEN
            || strlen(request_id) > PB_REPLAY_REQUEST_LEN) {
        release(c, response);
        return;
    }
    pb_replay_entry_t *e = &c->e[c->next++ % PB_REPLAY_ENTRIES];
    release(c, e->response);
    strcpy(e->actor_id, actor_id);       // lengths checked above
    strcpy(e->request_id, request_id);
    e->revision = revision;
    e->response = response;
}
