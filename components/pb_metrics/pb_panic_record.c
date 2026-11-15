// SPDX-License-Identifier: MIT
// pb_panic_record — see pb_panic_record.h.
#include "pb_panic_record.h"

#include <string.h>

uint32_t pb_panic_crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void pb_panic_seal(pb_panic_record_t *r)
{
    if (!r) return;
    r->magic = PB_PANIC_MAGIC;
    r->crc = pb_panic_crc32(r, offsetof(pb_panic_record_t, crc));
}

bool pb_panic_valid(const pb_panic_record_t *r)
{
    if (!r || r->magic != PB_PANIC_MAGIC) return false;
    if (r->crc != pb_panic_crc32(r, offsetof(pb_panic_record_t, crc))) return false;
    if (r->bt_n > PB_PANIC_BT_DEPTH || r->kind >= PB_PANIC_KINDS) return false;
    if (memchr(r->task, '\0', sizeof r->task) == NULL) return false;
    if (memchr(r->reason, '\0', sizeof r->reason) == NULL) return false;
    return true;
}

void pb_panic_copy_text(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    size_t n = 0;
    if (src) {
        for (; n + 1 < cap && src[n]; n++) {
            char c = src[n];
            dst[n] = (c >= 0x20 && c < 0x7f) ? c : '?';
        }
    }
    dst[n] = '\0';
}

const char *pb_panic_kind_str(unsigned kind)
{
    static const char *const names[PB_PANIC_KINDS] = {
        [PB_PANIC_KIND_DEBUG] = "debug",
        [PB_PANIC_KIND_IWDT]  = "int_wdt",
        [PB_PANIC_KIND_TWDT]  = "task_wdt",
        [PB_PANIC_KIND_ABORT] = "abort",
        [PB_PANIC_KIND_FAULT] = "fault",
    };
    return kind < PB_PANIC_KINDS ? names[kind] : "unknown";
}

static void put_hex32(char *out, uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 8; i++) out[2 + i] = hex[(v >> (28 - 4 * i)) & 0xF];
}

void pb_panic_bt_text(const pb_panic_record_t *r, char *buf, size_t cap)
{
    if (!buf || cap == 0) return;
    buf[0] = '\0';
    if (!r) return;
    size_t len = 0;
    unsigned n = r->bt_n <= PB_PANIC_BT_DEPTH ? r->bt_n : PB_PANIC_BT_DEPTH;
    for (unsigned i = 0; i < n; i++) {
        size_t need = (i ? 1 : 0) + 10;
        if (len + need + 1 > cap) break;
        if (i) buf[len++] = ' ';
        put_hex32(buf + len, r->bt[i]);
        len += 10;
    }
    buf[len] = '\0';
}
