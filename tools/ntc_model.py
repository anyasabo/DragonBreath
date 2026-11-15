#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Python port of the pb_ntc conversion (components/pb_ntc/pb_ntc.c).

This is a PORT for offline analysis. The firmware is the reference.
tests/test_capture_tools.py checks the table against pb_ntc.c and the values
against the C host tests.

    Rntc_kOhm = Rref * V / (Vsupply - V)       low-side divider, Vsupply = 3.3 V
    temp_C    = linear interpolation in the 114-entry R/T table (12..125 C)

Rref is 82 kOhm (board V1.0.1) or 33 kOhm (board V1.0). The same pin voltage
gives a different temperature with the other Rref. reinterpret_c() and
implied_rref_kohm() use this to test the hypothesis "the firmware uses the
wrong Rref". Standard library only.
"""
import math

VSUPPLY_V = 3.3
RT_TEMP_BASE = 12
# R/T table from pb_ntc.c (PB_RT_R_KOHM). Index i is (RT_TEMP_BASE + i) C.
RT_R_KOHM = (
    198.7, 189.4, 180.7, 172.4, 164.5, 157.0, 149.9, 143.2, 136.8, 130.7,
    124.9, 119.4, 114.2, 109.2, 104.5, 100.0, 95.7, 91.6, 87.8, 84.1,
    80.6, 77.2, 74.0, 70.9, 68.0, 65.3, 62.6, 60.1, 57.7, 55.4,
    53.2, 51.1, 49.1, 47.2, 45.3, 43.6, 41.9, 40.3, 38.8, 37.3,
    35.9, 34.5, 33.2, 32.0, 30.8, 29.7, 28.6, 27.6, 26.6, 25.6,
    24.7, 23.8, 23.0, 22.2, 21.4, 20.6, 19.9, 19.2, 18.6, 17.9,
    17.3, 16.7, 16.2, 15.6, 15.1, 14.6, 14.1, 13.6, 13.2, 12.8,
    12.4, 12.0, 11.6, 11.2, 10.9, 10.5, 10.2, 9.9, 9.6, 9.3,
    9.0, 8.7, 8.4, 8.2, 7.9, 7.7, 7.4, 7.2, 7.0, 6.8,
    6.6, 6.4, 6.2, 6.0, 5.9, 5.7, 5.5, 5.4, 5.2, 5.1,
    4.9, 4.8, 4.7, 4.5, 4.4, 4.3, 4.2, 4.1, 3.9, 3.8,
    3.7, 3.6, 3.5, 3.4,
)
T_MIN_C = float(RT_TEMP_BASE)
T_MAX_C = float(RT_TEMP_BASE + len(RT_R_KOHM) - 1)
RREF_CHOICES = (82, 33)


def r_to_temp_c(r_kohm):
    """Resistance (kOhm) to temperature (C). Same as rntc_to_temp_c() in pb_ntc.c."""
    if r_kohm >= RT_R_KOHM[0]:
        return T_MIN_C
    if r_kohm <= RT_R_KOHM[-1]:
        return T_MAX_C
    for i in range(len(RT_R_KOHM) - 1):
        rhi, rlo = RT_R_KOHM[i], RT_R_KOHM[i + 1]
        if rlo <= r_kohm <= rhi:
            return RT_TEMP_BASE + i + (rhi - r_kohm) / (rhi - rlo)
    return math.nan


def temp_to_r_kohm(temp_c):
    """Inverse of r_to_temp_c() inside the table. Outside, clamp to the table ends."""
    if temp_c <= T_MIN_C:
        return RT_R_KOHM[0]
    if temp_c >= T_MAX_C:
        return RT_R_KOHM[-1]
    i = int(math.floor(temp_c)) - RT_TEMP_BASE
    f = temp_c - math.floor(temp_c)
    return RT_R_KOHM[i] - f * (RT_R_KOHM[i] - RT_R_KOHM[i + 1])


def mv_to_temp_c(mv, rref_kohm):
    """Pin voltage (mV) to temperature (C), as pb_ntc_read() does after the ADC calibration."""
    v = mv / 1000.0
    if v <= 0.0 or v >= VSUPPLY_V:
        return math.nan            # the firmware reports OPEN here
    return r_to_temp_c(rref_kohm * v / (VSUPPLY_V - v))


def temp_to_mv(temp_c, rref_kohm):
    """Pin voltage (mV) that gives temp_c with this Rref. For replay and synthetic data."""
    r = temp_to_r_kohm(temp_c)
    return 1000.0 * VSUPPLY_V * r / (r + rref_kohm)


def is_clamped(temp_c, tol=0.05):
    """True if a reading sits at a table end. Then the true resistance is not known."""
    return temp_c is not None and (temp_c <= T_MIN_C + tol or temp_c >= T_MAX_C - tol)


def reinterpret_c(temp_read_c, rref_used_kohm, rref_true_kohm):
    """Temperature that the same pin voltage gives with the other Rref.

    R_read = Rref_used * V / (Vs - V) and R_true = Rref_true * V / (Vs - V),
    thus R_true = R_read * Rref_true / Rref_used.
    """
    return r_to_temp_c(temp_to_r_kohm(temp_read_c) * rref_true_kohm / rref_used_kohm)


def implied_rref_kohm(temp_read_c, temp_ref_c, rref_used_kohm):
    """The Rref that makes the reading equal to a reference temperature.

    If it is near the other board value (82 or 33), the firmware probably uses
    the wrong Rref. A value far from both points to another cause (sensor type,
    wiring, noise). Returns None for a clamped reading.
    """
    if is_clamped(temp_read_c) or is_clamped(temp_ref_c):
        return None
    return rref_used_kohm * temp_to_r_kohm(temp_ref_c) / temp_to_r_kohm(temp_read_c)


if __name__ == "__main__":
    import sys
    if len(sys.argv) != 3:
        print("usage: ntc_model.py MILLIVOLTS RREF_KOHM")
        sys.exit(2)
    print(f"{mv_to_temp_c(float(sys.argv[1]), float(sys.argv[2])):.2f}")
