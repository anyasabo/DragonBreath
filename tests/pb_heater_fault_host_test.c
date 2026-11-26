// SPDX-License-Identifier: MIT
// Host test for the SAFETY-CRITICAL boot-time fault-restore decision
// (pb_heater_fault_restore) — a pure header inline, verified without an NVS
// backend. This logic is what guarantees a device that latched a safety fault
// comes back up LOCKED after a power cycle, and fails SAFE (latched) whenever the
// persisted fault state cannot be read reliably.
#include "pb_heater.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

// Float-equality check for the foldback duty (values are exact-ish ratios of 5).
#define FEQ(a, b) (fabsf((a) - (b)) < 1e-4f)

int main(void)
{
    pb_fault_reason_t code;
    // The latch is fault_code alone (PB_FAULT_NONE = not latched). A legacy
    // fault_latch key (older firmware) can only ADD a latch.
#define RESTORE(open, lrd, lval, crd, cval) \
    pb_heater_fault_restore(PB_NVS_##open, PB_NVS_##lrd, (lval), PB_NVS_##crd, (cval), &code)

    // Fresh device: no namespace, or no fault keys -> NOT latched.
    CHECK(!RESTORE(NOT_FOUND, NOT_FOUND, 0, NOT_FOUND, 0) && code == PB_FAULT_NONE);
    CHECK(!RESTORE(OK, NOT_FOUND, 0, NOT_FOUND, 0) && code == PB_FAULT_NONE);
    // One key: NONE -> not latched; a valid cause -> latched with it.
    CHECK(!RESTORE(OK, NOT_FOUND, 0, OK, PB_FAULT_NONE) && code == PB_FAULT_NONE);
    CHECK(RESTORE(OK, NOT_FOUND, 0, OK, PB_FAULT_PTC_OVERTEMP) && code == PB_FAULT_PTC_OVERTEMP);
    // Out of range (corrupt) -> latch anyway, with a generic cause.
    CHECK(RESTORE(OK, NOT_FOUND, 0, OK, 250) && code == PB_FAULT_EMERGENCY);

    // Legacy keys. Latched with a valid code -> that code.
    CHECK(RESTORE(OK, OK, 1, OK, PB_FAULT_CHAMBER_OVERTEMP) && code == PB_FAULT_CHAMBER_OVERTEMP);
    // Legacy latch set, code NONE, missing or corrupt -> generic, still latched.
    CHECK(RESTORE(OK, OK, 1, OK, PB_FAULT_NONE) && code == PB_FAULT_EMERGENCY);
    CHECK(RESTORE(OK, OK, 7, NOT_FOUND, 0) && code == PB_FAULT_EMERGENCY);
    CHECK(RESTORE(OK, OK, 1, OK, 250) && code == PB_FAULT_EMERGENCY);
    // Legacy latch 0 and code NONE -> not latched.
    CHECK(!RESTORE(OK, OK, 0, OK, PB_FAULT_NONE) && code == PB_FAULT_NONE);
    // Legacy latch 0 but a cause in fault_code -> LATCHED. This is a legacy
    // clear that stopped part way, or a latch that this firmware wrote while
    // the legacy key stayed in flash. The legacy key must never hide it.
    CHECK(RESTORE(OK, OK, 0, OK, PB_FAULT_PTC_OVERTEMP) && code == PB_FAULT_PTC_OVERTEMP);

    // FAIL-SAFE: a genuine open error or a read error of either key -> latch.
    CHECK(RESTORE(ERROR, NOT_FOUND, 0, NOT_FOUND, 0) && code == PB_FAULT_NVS_UNREADABLE);
    CHECK(RESTORE(OK, NOT_FOUND, 0, ERROR, 0) && code == PB_FAULT_NVS_UNREADABLE);
    CHECK(RESTORE(OK, ERROR, 0, OK, PB_FAULT_NONE) && code == PB_FAULT_NVS_UNREADABLE);
    CHECK(RESTORE(OK, OK, 1, ERROR, 0) && code == PB_FAULT_NVS_UNREADABLE);
    CHECK(RESTORE(OK, ERROR, 0, ERROR, 0) && code == PB_FAULT_NVS_UNREADABLE);
#undef RESTORE

    puts("pb_heater fault-restore checks: PASS");

    // --- Element-foldback hysteresis (pb_heater_foldback_cut) -------------------
    // Per-Rref thresholds: 33k board gets the conservative pair, anything else the 82k
    // pair. Resolve both via the helper and exercise the hysteresis on each.
    float cut82, res82, cut33, res33;
    pb_heater_foldback_thresholds(82, &cut82, &res82);
    pb_heater_foldback_thresholds(33, &cut33, &res33);
    CHECK(cut82 == PB_HEATER_PTC_FOLDBACK_CUT_82K_C && res82 == PB_HEATER_PTC_FOLDBACK_RESUME_82K_C);
    CHECK(cut33 == PB_HEATER_PTC_FOLDBACK_CUT_33K_C && res33 == PB_HEATER_PTC_FOLDBACK_RESUME_33K_C);
    CHECK(cut33 < cut82);                                   // 33k board cuts earlier
    // Unknown/other Rref (incl. 0) falls back to the 82k pair.
    float cutx, resx; pb_heater_foldback_thresholds(0, &cutx, &resx);
    CHECK(cutx == cut82 && resx == res82);
    // Hysteresis behavior against each board's own thresholds.
    for (int b = 0; b < 2; b++) {
        float cut_c    = b ? cut33 : cut82;
        float resume_c = b ? res33 : res82;
        float band_mid = (cut_c + resume_c) * 0.5f;
        CHECK(pb_heater_foldback_cut(true, cut_c,        false, cut_c, resume_c) == true);   // at cut -> cut
        CHECK(pb_heater_foldback_cut(true, cut_c + 5.0f, false, cut_c, resume_c) == true);
        CHECK(pb_heater_foldback_cut(true, resume_c-0.1f, true, cut_c, resume_c) == false);  // below resume -> allow
        CHECK(pb_heater_foldback_cut(true, band_mid,      true,  cut_c, resume_c) == true);  // band holds prev
        CHECK(pb_heater_foldback_cut(true, band_mid,      false, cut_c, resume_c) == false);
        CHECK(pb_heater_foldback_cut(false, 0.0f,         true,  cut_c, resume_c) == true);  // bad read holds prev
        CHECK(pb_heater_foldback_cut(false, 0.0f,         false, cut_c, resume_c) == false);
        // Every board's cut/resume stays below the hard cutoff.
        CHECK(cut_c < PB_HEATER_PTC_CUTOFF_C && resume_c < cut_c);
    }
    // Effective-foldback resolver: 0/negative user override -> per-Rref default; a
    // positive override wins with resume = cut - band. Stays below the hard cutoff.
    float ec, er;
    pb_heater_effective_foldback(0.0f, 33, &ec, &er);    // auto -> 33k pair
    CHECK(ec == cut33 && er == res33);
    pb_heater_effective_foldback(-1.0f, 82, &ec, &er);   // <=0 also auto -> 82k pair
    CHECK(ec == cut82 && er == res82);
    pb_heater_effective_foldback(95.0f, 33, &ec, &er);   // override wins over per-Rref
    CHECK(ec == 95.0f && er == 95.0f - PB_HEATER_PTC_FOLDBACK_BAND_C);
    CHECK(ec < PB_HEATER_PTC_CUTOFF_C && er < ec);
    CHECK(PB_HEATER_FB_CUT_MAX_C < PB_HEATER_PTC_CUTOFF_C);   // override ceiling < hard cutoff
    puts("pb_heater element-foldback checks: PASS");

    // --- Local chamber soft limiter for remote regulation ----------------------
    CHECK(pb_heater_local_foldback_cut(true, 71.9f, false) == false);
    CHECK(pb_heater_local_foldback_cut(true, 72.0f, false) == true);
    CHECK(pb_heater_local_foldback_cut(true, 80.0f, false) == true);
    CHECK(pb_heater_local_foldback_cut(true, 72.0f, true) == true);   // hysteresis holds
    CHECK(pb_heater_local_foldback_cut(true, 66.9f, true) == false);
    CHECK(pb_heater_local_foldback_cut(false, 0.0f, true) == true);
    CHECK(pb_heater_local_foldback_cut(false, 0.0f, false) == false);
    CHECK(PB_HEATER_LOCAL_FOLDBACK_CUT_C < PB_HEATER_CHAMBER_MAX_C);
    CHECK(PB_HEATER_LOCAL_FOLDBACK_RESUME_C < PB_HEATER_LOCAL_FOLDBACK_CUT_C);
    puts("pb_heater local-chamber foldback checks: PASS");
    return 0;
}
