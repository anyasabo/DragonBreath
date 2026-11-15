// SPDX-License-Identifier: MIT
// Host test for the pure diagnostics of pb_metrics: the airflow signs
// (pb_metrics_airflow.c) and the panic breadcrumb record (pb_panic_record.c).
#include "pb_metrics_airflow.h"
#include "pb_panic_record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

#define TICK_US 500000
#define U PB_AIRFLOW_UNKNOWN

typedef struct {
    pb_airflow_t a;
    int64_t now;
} sim_t;

static void sim_init(sim_t *s)
{
    pb_airflow_init(&s->a);
    s->now = 1000000;
}

static void step(sim_t *s, bool heat, bool relay, bool fan, int32_t ptc, int32_t ch)
{
    pb_airflow_update(&s->a, s->now, heat, relay, fan, ptc, ch);
    s->now += TICK_US;
}

static void test_airflow_init(void)
{
    pb_airflow_t a;
    memset(&a, 0x5a, sizeof a);
    pb_airflow_init(&a);
    CHECK(a.rise_ratio_permille == PB_AIRFLOW_UNKNOWN && a.rise_samples == 0);
    CHECK(a.cool_tau_ms == 0 && a.cool_samples == 0 && !a.started);
    pb_airflow_init(NULL);
    pb_airflow_update(NULL, 0, true, true, true, 0, 0);
}

// Heat start, relay always on: the PTC rises 1.0 C and the outlet 0.4 C each
// second. After 45 s of on-time: ratio 400 permille.
static void test_rise_ratio(void)
{
    sim_t s;
    sim_init(&s);
    step(&s, false, false, false, 250, 240);           // idle
    int32_t ptc = 250, ch = 240;
    int ticks = 0;
    for (; ticks < 200 && s.a.rise_samples == 0; ticks++) {
        step(&s, true, true, true, ptc, ch);
        ptc += 5;                                       // 0.5 C each 0.5 s tick
        ch += 2;
    }
    CHECK(s.a.rise_samples == 1);
    // 45 s of on-time = 90 intervals after the start tick: 91 ticks.
    CHECK(ticks == 91);
    CHECK(s.a.rise_ratio_permille == 400);
    // The rest of this heat gives no more samples.
    for (int i = 0; i < 200; i++) step(&s, true, true, true, ptc, ch);
    CHECK(s.a.rise_samples == 1);
}

// Relay off time does not count: 50 % duty needs about 90 s for 45 s on-time.
static void test_rise_counts_only_on_time(void)
{
    sim_t s;
    sim_init(&s);
    int ticks = 0;
    int32_t ptc = 300, ch = 250;
    for (; ticks < 400 && s.a.rise_samples == 0; ticks++) {
        bool relay = (ticks / 4) % 2 == 0;              // 2 s on, 2 s off
        step(&s, true, relay, true, ptc, ch);
        if (relay) { ptc += 8; ch += 1; }
    }
    CHECK(s.a.rise_samples == 1);
    CHECK(ticks > 170 && ticks < 190);
    CHECK(s.a.rise_ratio_permille == 125);              // 1 / 8
}

static void test_rise_aborts(void)
{
    sim_t s;
    sim_init(&s);
    // Heat stops before 45 s of on-time: no sample.
    for (int i = 0; i < 40; i++) step(&s, true, true, true, 300 + 5 * i, 250);
    step(&s, false, false, true, 500, 260);
    for (int i = 0; i < 200; i++) step(&s, false, false, true, 500, 260);
    CHECK(s.a.rise_samples == 0 && !s.a.rise_active);
    // A sensor fault in the window: no sample.
    for (int i = 0; i < 60; i++) step(&s, true, true, true, 300 + 5 * i, 250);
    step(&s, true, true, true, U, 250);
    for (int i = 0; i < 200; i++) step(&s, true, true, true, 900, 300);
    CHECK(s.a.rise_samples == 0);
    // The PTC does not rise 5 C (a dead element): no sample, the old value stays.
    sim_init(&s);
    step(&s, false, false, false, 250, 240);
    for (int i = 0; i < 200; i++) step(&s, true, true, true, 250 + (i > 100 ? 49 : 0), 240);
    CHECK(s.a.rise_samples == 0 && s.a.rise_ratio_permille == PB_AIRFLOW_UNKNOWN);
    // A heat start with an unknown sensor does not start a window.
    sim_init(&s);
    step(&s, true, true, true, U, 240);
    for (int i = 0; i < 200; i++) step(&s, true, true, true, 300 + 5 * i, 240);
    CHECK(s.a.rise_samples == 0);
}

// A long gap between ticks counts as PB_AIRFLOW_MAX_STEP_MS only.
static void test_rise_gap_is_bounded(void)
{
    pb_airflow_t a;
    pb_airflow_init(&a);
    pb_airflow_update(&a, 1000000, true, true, true, 300, 250);
    pb_airflow_update(&a, 1000000 + 60000000, true, true, true, 900, 300);   // 60 s gap
    CHECK(a.rise_on_ms == PB_AIRFLOW_MAX_STEP_MS && a.rise_samples == 0);
    pb_airflow_update(&a, 500000, true, true, true, 900, 300);                // clock back
    CHECK(a.rise_on_ms == PB_AIRFLOW_MAX_STEP_MS);
}

// After the relay goes off with the fan on, the PTC excess decays. Excess 60 C
// at the start, minus 1 % each 0.5 s: 1/e after about 50 s.
static void test_cooldown_tau(void)
{
    sim_t s;
    sim_init(&s);
    step(&s, true, true, true, 900, 300);
    int32_t excess = 600;
    int ticks = 0;
    for (; ticks < 1000 && s.a.cool_samples == 0; ticks++) {
        step(&s, true, false, true, 300 + excess, 300);
        excess -= excess / 100 + 1;
    }
    CHECK(s.a.cool_samples == 1);
    // excess <= 600 * 0.368 = 220.8 after n ticks; tau = (n - 1) * 0.5 s.
    CHECK(s.a.cool_tau_ms >= 40000 && s.a.cool_tau_ms <= 60000);
    CHECK(s.a.cool_tau_ms % 500 == 0);
}

static void test_cooldown_aborts(void)
{
    sim_t s;
    sim_init(&s);
    // The relay goes on again before 1/e: no sample.
    step(&s, true, true, true, 900, 300);
    for (int i = 0; i < 10; i++) step(&s, true, false, true, 890 - i, 300);
    step(&s, true, true, true, 880, 300);
    CHECK(!s.a.cool_active && s.a.cool_samples == 0);
    // Fan off: no start.
    step(&s, true, false, false, 880, 300);
    CHECK(!s.a.cool_active);
    // Excess below 15 C: no start.
    step(&s, true, true, true, 440, 300);
    step(&s, true, false, true, 440, 300);
    CHECK(!s.a.cool_active);
    // The fan stops during the decay: abort.
    step(&s, true, true, true, 900, 300);
    step(&s, true, false, true, 900, 300);
    CHECK(s.a.cool_active);
    step(&s, true, false, false, 850, 300);
    CHECK(!s.a.cool_active);
    // A decay that never reaches 1/e stops after PB_AIRFLOW_COOL_MAX_MS.
    step(&s, true, true, true, 900, 300);
    step(&s, true, false, true, 900, 300);
    for (unsigned i = 0; i < PB_AIRFLOW_COOL_MAX_MS / 500 + 2; i++)
        step(&s, true, false, true, 899, 300);
    CHECK(!s.a.cool_active && s.a.cool_samples == 0);
}

// ---- panic record ------------------------------------------------------------

static void test_crc(void)
{
    CHECK(pb_panic_crc32("123456789", 9) == 0xCBF43926u);   // the standard check value
    CHECK(pb_panic_crc32("", 0) == 0);
}

static pb_panic_record_t good_record(void)
{
    pb_panic_record_t r;
    memset(&r, 0, sizeof r);
    pb_panic_copy_text(r.task, sizeof r.task, "pb_control");
    r.pc = 0x42001234;
    r.ra = 0x42005678;
    r.kind = PB_PANIC_KIND_FAULT;
    pb_panic_copy_text(r.reason, sizeof r.reason, "Load access fault");
    pb_panic_seal(&r);
    return r;
}

static void test_record_valid(void)
{
    pb_panic_record_t r = good_record();
    CHECK(pb_panic_valid(&r));
    CHECK(r.magic == PB_PANIC_MAGIC);

    // Each changed byte before the CRC makes the record not valid.
    for (size_t i = 0; i < offsetof(pb_panic_record_t, crc); i++) {
        pb_panic_record_t c = r;
        ((uint8_t *)&c)[i] ^= 0x01;
        CHECK(!pb_panic_valid(&c));
    }
    // Random power-on content.
    pb_panic_record_t z;
    memset(&z, 0xA5, sizeof z);
    CHECK(!pb_panic_valid(&z));
    memset(&z, 0, sizeof z);
    CHECK(!pb_panic_valid(&z));
    CHECK(!pb_panic_valid(NULL));

    // A sealed record with out-of-range fields is not valid.
    pb_panic_record_t c = r;
    c.bt_n = PB_PANIC_BT_DEPTH + 1;
    pb_panic_seal(&c);
    CHECK(!pb_panic_valid(&c));
    c = r;
    c.kind = PB_PANIC_KINDS;
    pb_panic_seal(&c);
    CHECK(!pb_panic_valid(&c));
    c = r;
    memset(c.task, 'x', sizeof c.task);                 // no terminator
    pb_panic_seal(&c);
    CHECK(!pb_panic_valid(&c));
    c = r;
    memset(c.reason, 'x', sizeof c.reason);
    pb_panic_seal(&c);
    CHECK(!pb_panic_valid(&c));
    // The reported flag is part of the CRC: a reseal is necessary.
    c = r;
    c.reported = 1;
    CHECK(!pb_panic_valid(&c));
    pb_panic_seal(&c);
    CHECK(pb_panic_valid(&c));
}

static void test_copy_text(void)
{
    char buf[8];
    pb_panic_copy_text(buf, sizeof buf, "abc\ndef\x01ghi");
    CHECK(strcmp(buf, "abc?def") == 0);
    pb_panic_copy_text(buf, sizeof buf, NULL);
    CHECK(buf[0] == '\0');
    pb_panic_copy_text(buf, 1, "abc");
    CHECK(buf[0] == '\0');
    pb_panic_copy_text(NULL, 4, "abc");
    char hi[4];
    pb_panic_copy_text(hi, sizeof hi, "\xc3\xa9x");     // non-ASCII bytes
    CHECK(strcmp(hi, "??x") == 0);
}

static void test_kind_and_bt(void)
{
    CHECK(strcmp(pb_panic_kind_str(PB_PANIC_KIND_TWDT), "task_wdt") == 0);
    CHECK(strcmp(pb_panic_kind_str(PB_PANIC_KIND_IWDT), "int_wdt") == 0);
    CHECK(strcmp(pb_panic_kind_str(PB_PANIC_KIND_ABORT), "abort") == 0);
    CHECK(strcmp(pb_panic_kind_str(99), "unknown") == 0);

    pb_panic_record_t r = good_record();
    char bt[PB_PANIC_BT_TEXT + 1];
    pb_panic_bt_text(&r, bt, sizeof bt);
    CHECK(bt[0] == '\0');
    r.bt_n = 3;
    r.bt[0] = 0x4200000a;
    r.bt[1] = 0x42abcdef;
    r.bt[2] = 0;
    pb_panic_bt_text(&r, bt, sizeof bt);
    CHECK(strcmp(bt, "0x4200000a 0x42abcdef 0x00000000") == 0);
    r.bt_n = PB_PANIC_BT_DEPTH;
    for (int i = 0; i < PB_PANIC_BT_DEPTH; i++) r.bt[i] = 0xffffffffu;
    pb_panic_bt_text(&r, bt, sizeof bt);
    CHECK(strlen(bt) == PB_PANIC_BT_DEPTH * 11 - 1);
    char small[15];
    pb_panic_bt_text(&r, small, sizeof small);              // only whole entries
    CHECK(strcmp(small, "0xffffffff") == 0);
    r.bt_n = 200;                                           // out of range: cut to the depth
    pb_panic_bt_text(&r, bt, sizeof bt);
    CHECK(strlen(bt) == PB_PANIC_BT_DEPTH * 11 - 1);
    pb_panic_bt_text(NULL, bt, sizeof bt);
    CHECK(bt[0] == '\0');
}

int main(void)
{
    test_airflow_init();
    test_rise_ratio();
    test_rise_counts_only_on_time();
    test_rise_aborts();
    test_rise_gap_is_bounded();
    test_cooldown_tau();
    test_cooldown_aborts();
    test_crc();
    test_record_valid();
    test_copy_text();
    test_kind_and_bt();
    printf("pb_metrics diagnostics host test: PASS\n");
    return 0;
}
