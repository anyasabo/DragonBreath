// SPDX-License-Identifier: MIT
// Host test for pb_status_line: the status line out of the control task.
// It checks that the integer formatter gives the same text as the old
// snprintf("%.1f") code in app_main, and the repeat collapse.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pb_status_line.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b)) != 0) { \
    fprintf(stderr, "FAIL %s:%d:\n  got  \"%s\"\n  want \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
    failures++; } } while (0)

// The old app_main code, as a reference (float values, %.1f and %.0f).
static void old_line(char *buf, size_t cap, bool bambu, bool key, float target, float ch, float ptc,
                     float bed, float bed_t, float chsrc)
{
    if (bambu) {
        if (key)
            snprintf(buf, cap,
                "rev=%lu mode=%s source=%s target=%.0fC heater=%s | chamber=%.0fC ptc=%.0fC | "
                "wifi=%d src=bambu link=%d print=%s filament=%s bed=%.0f/%.0f chsrc=%.0f",
                7ul, "auto", "web", target, "ON", ch, ptc, 3, 2, "yes", "PETG", bed, bed_t, chsrc);
        else
            snprintf(buf, cap,
                "rev=%lu mode=%s source=%s target=%.0fC heater=%s | chamber=%.1fC ptc=%.1fC | "
                "wifi=%d src=bambu link=%d print=%s filament=%s bed=%.1f/%.1f chsrc=%.1f",
                7ul, "auto", "web", target, "ON", ch, ptc, 3, 2, "yes", "PETG", bed, bed_t, chsrc);
    } else {
        if (key)
            snprintf(buf, cap,
                "rev=%lu mode=%s source=%s target=%.0fC heater=%s | chamber=%.0fC ptc=%.0fC | "
                "wifi=%d src=%s mk=%d printer=%s bed=%.0f",
                7ul, "auto", "web", target, "ON", ch, ptc, 3, "prusa", 0, "printing", bed);
        else
            snprintf(buf, cap,
                "rev=%lu mode=%s source=%s target=%.0fC heater=%s | chamber=%.1fC ptc=%.1fC | "
                "wifi=%d src=%s mk=%d printer=%s bed=%.1f",
                7ul, "auto", "web", target, "ON", ch, ptc, 3, "prusa", 0, "printing", bed);
    }
}

static pb_status_sample_t sample(bool bambu, float target, float ch, float ptc,
                                 float bed, float bed_t, float chsrc)
{
    pb_status_sample_t s = {
        .revision = 7, .mode = "auto", .source = "web",
        .target = pb_status_temp(target), .heater_on = true,
        .chamber = pb_status_temp(ch), .ptc = pb_status_temp(ptc),
        .wifi = 3, .bambu = bambu, .link = bambu ? 2 : 0, .printing = true,
        .bed = pb_status_temp(bed), .bed_target = pb_status_temp(bed_t),
        .chsrc = pb_status_temp(chsrc), .src = "prusa", .printer = "printing",
        .zc_n = 100, .zc_dt_us = 10000, .zc_rejected = 1,
    };
    strcpy(s.filament, "PETG");
    return s;
}

// The text of one value: the new formatter against printf.
static void check_one(float v)
{
    char got[PB_STATUS_LINE_MAX], want[PB_STATUS_LINE_MAX];
    for (int key = 0; key < 2; key++) {
        pb_status_sample_t s = sample(false, 40.0f, v, 20.0f, 60.0f, 0, 0);
        pb_status_format(&s, key, got, sizeof got);
        old_line(want, sizeof want, false, key, 40.0f, v, 20.0f, 60.0f, 0, 0);
        CHECK_STR(got, want);
    }
}

static void test_tenths(void)
{
    pb_status_temp_t t = pb_status_temp(21.54f);
    CHECK(t.known && !t.neg && t.dc == 215 && t.whole == 22);
    t = pb_status_temp(130.46f);
    CHECK(t.dc == 1305 && t.whole == 130);         // one rounding, not two
    t = pb_status_temp(-5.2f);
    CHECK(t.known && t.neg && t.dc == 52 && t.whole == 5);
    // Ties on the exact binary value round half to even, as printf.
    CHECK(pb_status_temp(0.25f).dc == 2);           // 0.25 is exact: "0.2"
    CHECK(pb_status_temp(0.75f).dc == 8);           // "0.8"
    CHECK(pb_status_temp(0.35f).dc == 3);           // 0.3499999940...: "0.3"
    CHECK(pb_status_temp(42.5f).whole == 42);       // "42"
    CHECK(pb_status_temp(43.5f).whole == 44);       // "44"
    t = pb_status_temp(-0.04f);
    CHECK(t.known && t.neg && t.dc == 0);           // "-0.0"
    t = pb_status_temp(-0.0f);
    CHECK(t.known && t.neg && t.dc == 0 && t.whole == 0);
    CHECK(pb_status_temp(1e-45f).dc == 0);          // subnormal
    CHECK(pb_status_temp(100000.0f).dc == 1000000);
    CHECK(!pb_status_temp(NAN).known);
    CHECK(!pb_status_temp(INFINITY).known);
    CHECK(!pb_status_temp(-INFINITY).known);
    CHECK(!pb_status_temp(1e9f).known);
    CHECK(!pb_status_temp(100000.01f).known);
}

// Ties and values near ties: each k / 20 and k / 200 degrees in -50 .. 150 C,
// the float next to each one, and the tie cases of the review.
static void test_ties_same_as_printf(void)
{
    static const float cases[] = { 0.35f, 42.5f, -0.04f, 0.25f, -0.25f, -0.5f, 0.5f,
                                   1.5f, 2.5f, -2.5f, 0.05f, -0.05f, 99.95f, 0.0f, -0.0f };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) check_one(cases[i]);
    for (int k = -10000; k <= 30000 && failures <= 5; k++) {
        float v = (float)k / 200.0f;
        check_one(v);
        check_one(nextafterf(v, 1e9f));
        check_one(nextafterf(v, -1e9f));
    }
}

static void test_same_text_as_old_code(void)
{
    char got[PB_STATUS_LINE_MAX], want[PB_STATUS_LINE_MAX];
    srand(1);
    int compared = 0;
    for (int i = 0; i < 20000; i++) {
        float v[6];
        for (int k = 0; k < 6; k++)
            v[k] = (float)(rand() % 200000) / 1000.0f - 40.0f;   // -40 .. 160 C, no filter
        v[0] = (float)(rand() % 71);                             // targets are whole degrees
        for (int b = 0; b < 2; b++) {
            for (int key = 0; key < 2; key++) {
                pb_status_sample_t s = sample(b, v[0], v[1], v[2], v[3], v[4], v[5]);
                pb_status_format(&s, key, got, sizeof got);
                old_line(want, sizeof want, b, key, v[0], v[1], v[2], v[3], v[4], v[5]);
                CHECK_STR(got, want);
                compared++;
                if (failures > 5) return;
            }
        }
    }
    CHECK(compared == 80000);
}

static void test_nan_and_negative(void)
{
    char got[PB_STATUS_LINE_MAX];
    pb_status_sample_t s = sample(false, 0.0f, NAN, -0.5f, -12.34f, 0, 0);
    s.printer = NULL;
    pb_status_format(&s, false, got, sizeof got);
    CHECK_STR(got, "rev=7 mode=auto source=web target=0C heater=ON | chamber=nanC ptc=-0.5C | "
                   "wifi=3 src=prusa mk=0 printer=? bed=-12.3");
    pb_status_format(&s, true, got, sizeof got);
    CHECK_STR(got, "rev=7 mode=auto source=web target=0C heater=ON | chamber=nanC ptc=-0C | "
                   "wifi=3 src=prusa mk=0 printer=? bed=-12");
    s = sample(true, 50.0f, 20.0f, 20.0f, 60.0f, 60.0f, NAN);
    s.filament[0] = '\0';
    pb_status_format(&s, false, got, sizeof got);
    CHECK_STR(got, "rev=7 mode=auto source=web target=50C heater=ON | chamber=20.0C ptc=20.0C | "
                   "wifi=3 src=bambu link=2 print=yes filament=- bed=60.0/60.0 chsrc=nan");
}

static char emitted[8][PB_STATUS_LINE_MAX + 96];
static int n_emitted;
static void emit(void *ctx, const char *line)
{
    (void)ctx;
    if (n_emitted < 8) snprintf(emitted[n_emitted], sizeof emitted[0], "%s", line);
    n_emitted++;
}

static void test_repeat_collapse(void)
{
    pb_status_dedup_t st;
    memset(&st, 0, sizeof st);
    pb_status_sample_t a = sample(false, 40.0f, 30.04f, 80.0f, 60.0f, 0, 0);
    pb_status_sample_t a2 = sample(false, 40.0f, 30.21f, 80.0f, 60.0f, 0, 0);  // same whole degrees
    pb_status_sample_t b = sample(false, 40.0f, 31.0f, 80.0f, 60.0f, 0, 0);

    n_emitted = 0;
    pb_status_feed(&st, &a, emit, NULL);
    CHECK(n_emitted == 1);
    CHECK_STR(emitted[0], "rev=7 mode=auto source=web target=40C heater=ON | chamber=30.0C ptc=80.0C | "
                          "wifi=3 src=prusa mk=0 printer=printing bed=60.0 | ZC n=100 dt=10000us rejected=1");
    n_emitted = 0;
    for (int i = 1; i < PB_STATUS_REPEAT_FLUSH; i++) pb_status_feed(&st, &a2, emit, NULL);
    CHECK(n_emitted == 0);
    pb_status_feed(&st, &a2, emit, NULL);          // repeat 30: the liveness line
    CHECK(n_emitted == 1);
    CHECK_STR(emitted[0], "rev=7 mode=auto source=web target=40C heater=ON | chamber=30.2C ptc=80.0C | "
                          "wifi=3 src=prusa mk=0 printer=printing bed=60.0 | repeated 30x | "
                          "ZC n=100 dt=10000us rejected=1");
    n_emitted = 0;
    pb_status_feed(&st, &a2, emit, NULL);
    pb_status_feed(&st, &b, emit, NULL);           // a change after 31 repeats
    CHECK(n_emitted == 2);
    CHECK_STR(emitted[0], "(previous line repeated 31x)");
    CHECK(strstr(emitted[1], "chamber=31.0C") != NULL);
    n_emitted = 0;
    pb_status_feed(&st, &a, emit, NULL);           // a change with no repeats: one line
    CHECK(n_emitted == 1);
    // NULL arguments do nothing.
    pb_status_feed(NULL, &a, emit, NULL);
    pb_status_feed(&st, NULL, emit, NULL);
    pb_status_feed(&st, &a, NULL, NULL);
    CHECK(n_emitted == 1);
}

static void test_truncation(void)
{
    char small[20];
    pb_status_sample_t s = sample(true, 40.0f, 30.0f, 80.0f, 60.0f, 60.0f, 30.0f);
    pb_status_format(&s, false, small, sizeof small);
    CHECK(strlen(small) == sizeof small - 1);
    pb_status_format(&s, false, small, 0);       // no write
    pb_status_format(NULL, false, small, sizeof small);
    CHECK(small[0] == '\0');
}

int main(void)
{
    test_tenths();
    test_ties_same_as_printf();
    test_same_text_as_old_code();
    test_nan_and_negative();
    test_repeat_collapse();
    test_truncation();
    if (failures) {
        fprintf(stderr, "pb_status_line: %d failure(s)\n", failures);
        return 1;
    }
    printf("pb_status_line host test: OK\n");
    return 0;
}
