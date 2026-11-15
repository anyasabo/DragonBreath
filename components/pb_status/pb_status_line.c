// SPDX-License-Identifier: MIT
// pb_status_line — see pb_status_line.h.
#include "pb_status_line.h"

#include <stdio.h>
#include <string.h>

static int32_t round_away(float t)
{
    return (int32_t)(t >= 0.0f ? t + 0.5f : t - 0.5f);
}

pb_status_temp_t pb_status_temp(float c)
{
    pb_status_temp_t t = { PB_STATUS_UNKNOWN, PB_STATUS_UNKNOWN };
    // c != c: NaN. The range check also rejects +-Inf.
    if (c != c || c > 100000.0f || c < -100000.0f) return t;
    t.dc = round_away(c * 10.0f);
    t.whole = round_away(c);
    return t;
}

// "21.5" (tenths) or "22" (whole), "nan" if unknown.
static const char *fmt_temp(char *out, size_t cap, pb_status_temp_t t, bool whole)
{
    int32_t v = whole ? t.whole : t.dc;
    if (v == PB_STATUS_UNKNOWN) {
        snprintf(out, cap, "nan");
        return out;
    }
    const char *sign = v < 0 ? "-" : "";
    uint32_t a = v < 0 ? (uint32_t)0 - (uint32_t)v : (uint32_t)v;
    if (whole)
        snprintf(out, cap, "%s%lu", sign, (unsigned long)a);
    else
        snprintf(out, cap, "%s%lu.%lu", sign, (unsigned long)(a / 10), (unsigned long)(a % 10));
    return out;
}

static const char *str(const char *s)
{
    return s ? s : "?";
}

void pb_status_format(const pb_status_sample_t *s, bool whole, char *buf, size_t cap)
{
    if (!buf || cap == 0) return;
    buf[0] = '\0';
    if (!s) return;
    char tgt[16], ch[16], ptc[16], bed[16];
    fmt_temp(tgt, sizeof tgt, s->target, true);
    fmt_temp(ch, sizeof ch, s->chamber, whole);
    fmt_temp(ptc, sizeof ptc, s->ptc, whole);
    fmt_temp(bed, sizeof bed, s->bed, whole);
    if (s->bambu) {
        char bt[16], cs[16];
        fmt_temp(bt, sizeof bt, s->bed_target, whole);
        fmt_temp(cs, sizeof cs, s->chsrc, whole);
        snprintf(buf, cap,
            "rev=%lu mode=%s source=%s target=%sC heater=%s | chamber=%sC ptc=%sC | "
            "wifi=%d src=bambu link=%d print=%s filament=%s bed=%s/%s chsrc=%s",
            (unsigned long)s->revision, str(s->mode), str(s->source), tgt,
            s->heater_on ? "ON" : "off", ch, ptc,
            s->wifi, s->link, s->printing ? "yes" : "no",
            s->filament[0] ? s->filament : "-", bed, bt, cs);
    } else {
        snprintf(buf, cap,
            "rev=%lu mode=%s source=%s target=%sC heater=%s | chamber=%sC ptc=%sC | "
            "wifi=%d src=%s mk=%d printer=%s bed=%s",
            (unsigned long)s->revision, str(s->mode), str(s->source), tgt,
            s->heater_on ? "ON" : "off", ch, ptc,
            s->wifi, str(s->src), s->link, str(s->printer), bed);
    }
}

void pb_status_feed(pb_status_dedup_t *st, const pb_status_sample_t *s,
                    pb_status_emit_fn emit, void *ctx)
{
    if (!st || !s || !emit) return;
    // Static: the buffers are too large for a small task stack. Only the
    // status task (or one host test) calls this function.
    static char line[PB_STATUS_LINE_MAX];
    static char key[PB_STATUS_LINE_MAX];
    static char out[PB_STATUS_LINE_MAX + 96];
    pb_status_format(s, false, line, sizeof line);
    pb_status_format(s, true, key, sizeof key);

    if (strcmp(key, st->last_key) == 0) {
        st->repeats++;
        if (st->repeats % PB_STATUS_REPEAT_FLUSH == 0) {
            snprintf(out, sizeof out, "%s | repeated %lux | ZC n=%lu dt=%luus rejected=%lu",
                     line, (unsigned long)st->repeats, (unsigned long)s->zc_n,
                     (unsigned long)s->zc_dt_us, (unsigned long)s->zc_rejected);
            emit(ctx, out);
        }
        return;
    }
    if (st->repeats > 0) {
        snprintf(out, sizeof out, "(previous line repeated %lux)", (unsigned long)st->repeats);
        emit(ctx, out);
    }
    snprintf(out, sizeof out, "%s | ZC n=%lu dt=%luus rejected=%lu",
             line, (unsigned long)s->zc_n, (unsigned long)s->zc_dt_us,
             (unsigned long)s->zc_rejected);
    emit(ctx, out);
    memcpy(st->last_key, key, sizeof st->last_key);
    st->last_key[sizeof st->last_key - 1] = '\0';
    st->repeats = 0;
}
