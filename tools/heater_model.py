# SPDX-License-Identifier: MIT
"""Python MODEL of the DragonBreath heater decisions. This is NOT the firmware.

It copies the decision rules of pb_heater_tick() (components/pb_heater/pb_heater.c)
and of the constants in pb_heater.h, for the bang-bang method only:

  1. trips: PTC >= 105 C, chamber >= 85 C, a sensor not OK while armed -> latch off
  2. not armed or latched -> off, latches reset
  3. element foldback: cut at PTC >= cut, hold off until PTC < resume
  4. bang-bang: heat below target - 1 C, stop at target

The firmware uses the instantaneous sample. /api/v2/state reports a 5-sample
moving average. Thus a replay of a capture can disagree near a threshold. The
replay counts such samples as "ambiguous", not as a mismatch.

FirstHeatModel copies pb_heater_first_heat_eval() of the branch
feat/first-heat-check (in progress, thresholds provisional). The window rules
(start, peak, restart) are an approximation of that branch.

If you change a constant in the firmware, change it here too.
tests/test_capture_tools.py compares the constants with pb_heater.h.
"""

PTC_CUTOFF_C = 105.0             # PB_HEATER_PTC_CUTOFF_C
CHAMBER_MAX_C = 85.0             # PB_HEATER_CHAMBER_MAX_C
HYSTERESIS_C = 1.0               # PB_HEATER_HYSTERESIS_C
FOLDBACK_82K = (102.0, 99.0)     # PB_HEATER_PTC_FOLDBACK_CUT/RESUME_82K_C
FOLDBACK_33K = (99.0, 96.0)      # PB_HEATER_PTC_FOLDBACK_CUT/RESUME_33K_C
FOLDBACK_BAND_C = 3.0            # PB_HEATER_PTC_FOLDBACK_BAND_C
COOL_RELEASE_C = 40.0            # PB_HEATER_COOL_RELEASE_C_DEFAULT
PURGE_BAND_C = 3.0               # purge engages one band above cool_release

# feat/first-heat-check, PROVISIONAL
FH_MIN_ON_MS = 12000
FH_MAX_ON_MS = 30000
FH_MAX_WALL_MS = 120000
FH_PASS_RISE_DC = 30
FH_CROSS_RISE_DC = 30
FH_HOT_START_DC = 700


def foldback_thresholds(rref_kohm, fb_cut_c=0.0):
    """pb_heater_effective_foldback(): a user cut wins, else the per-Rref pair."""
    if fb_cut_c and fb_cut_c > 0:
        return fb_cut_c, fb_cut_c - FOLDBACK_BAND_C
    return FOLDBACK_33K if rref_kohm == 33 else FOLDBACK_82K


class BangBangModel:
    """One instance follows one heater. step() is one control tick."""

    def __init__(self, rref_kohm=82, fb_cut_c=0.0):
        self.cut_c, self.resume_c = foldback_thresholds(rref_kohm, fb_cut_c)
        self.heat_intent = False
        self.fb_cut = False
        self.latched = False
        self.trip = None

    def step(self, chamber_c, ptc_c, target_c, chamber_ok=True, ptc_ok=True):
        """Return (relay on, constraint). target_c <= 0 or None means not armed."""
        armed = target_c is not None and target_c > 0
        trip = None
        if ptc_ok and ptc_c is not None and ptc_c >= PTC_CUTOFF_C:
            trip = "ptc_overtemp"
        elif chamber_ok and chamber_c is not None and chamber_c >= CHAMBER_MAX_C:
            trip = "chamber_overtemp"
        elif armed and not (chamber_ok and ptc_ok):
            trip = "sensor"
        if trip:
            self.latched, self.trip = True, trip
            return False, "off"
        if self.latched or not armed:
            self.heat_intent = self.fb_cut = False
            return False, "off"
        if ptc_c >= self.cut_c:
            self.fb_cut = True
        elif ptc_c < self.resume_c:
            self.fb_cut = False
        if chamber_c < target_c - HYSTERESIS_C:
            self.heat_intent = True
        elif chamber_c >= target_c:
            self.heat_intent = False
        drive = self.heat_intent and not self.fb_cut
        if self.fb_cut:
            constraint = "element_foldback"
        elif chamber_c >= target_c:
            constraint = "target_reached"
        else:
            constraint = "none"
        return drive, constraint

    def clear(self):
        self.latched, self.trip = False, None


def c_to_dc(c):
    """pb_heater_c_to_dc(): C to integer tenths, half away from zero."""
    c = max(-100.0, min(300.0, c))
    return int(c * 10.0 + (0.5 if c >= 0 else -0.5))


def fh_eval(on_ms, ptc_rise_dc, ch_rise_dc, pass_dc=FH_PASS_RISE_DC,
            cross_dc=FH_CROSS_RISE_DC, min_on_ms=FH_MIN_ON_MS, max_on_ms=FH_MAX_ON_MS):
    """pb_heater_first_heat_eval(): 'wait', 'pass', 'crossed' or 'no_rise'."""
    if on_ms < min_on_ms:
        return "wait"
    if ch_rise_dc >= cross_dc and ch_rise_dc > ptc_rise_dc:
        return "crossed"
    if ptc_rise_dc >= pass_dc and ptc_rise_dc > 2 * ch_rise_dc:
        return "pass"
    if on_ms >= max_on_ms:
        return "no_rise"
    return "wait"


class FirstHeatModel:
    """Approximate first-heat window: opens at the first relay ON (PTC below the hot-start
    limit), counts ON time, takes rises as peak minus start, decides with fh_eval()."""

    def __init__(self, **thresholds):
        self.th = thresholds
        self.state = "idle"
        self.t_open = None
        self.on_ms = 0.0
        self.p0 = self.c0 = None
        self.p_peak = self.c_peak = None
        self.t_prev = None
        self.relay_prev = 0
        self.decided_at_on_ms = None

    def feed(self, t_s, relay, ptc_c, ch_c):
        if self.state in ("pass", "crossed", "no_rise") or ptc_c is None or ch_c is None:
            self.t_prev, self.relay_prev = t_s, relay
            return self.state
        if self.state == "idle":
            if relay and not self.relay_prev:
                if c_to_dc(ptc_c) < FH_HOT_START_DC:
                    self.state, self.t_open, self.on_ms = "armed", t_s, 0.0
                    self.p0 = self.p_peak = ptc_c
                    self.c0 = self.c_peak = ch_c
        else:
            if self.relay_prev and self.t_prev is not None:
                self.on_ms += (t_s - self.t_prev) * 1000.0
            self.p_peak = max(self.p_peak, ptc_c)
            self.c_peak = max(self.c_peak, ch_c)
            r = fh_eval(self.on_ms, c_to_dc(self.p_peak - self.p0),
                        c_to_dc(self.c_peak - self.c0), **self.th)
            if r != "wait":
                self.state, self.decided_at_on_ms = r, self.on_ms
            elif (t_s - self.t_open) * 1000.0 > FH_MAX_WALL_MS:
                self.state = "idle"              # restart on the next relay ON
            else:
                self.state = "eval" if self.on_ms >= FH_MIN_ON_MS else "armed"
        self.t_prev, self.relay_prev = t_s, relay
        return self.state

    @property
    def ptc_rise_c(self):
        return None if self.p0 is None else self.p_peak - self.p0

    @property
    def ch_rise_c(self):
        return None if self.c0 is None else self.c_peak - self.c0
