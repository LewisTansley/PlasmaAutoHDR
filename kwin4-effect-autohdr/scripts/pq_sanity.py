#!/usr/bin/env python3
"""Offline sanity checks for PQ-on-Y perceptual remapping (mirrors autohdr_config.cpp)."""

from __future__ import annotations

import math
import sys

PQ_N = 2610.0 / 4096.0 / 4.0
PQ_RCP_N = 1.0 / PQ_N
PQ_M = 2523.0 / 4096.0 * 128.0
PQ_RCP_M = 1.0 / PQ_M
PQ_C1 = 3424.0 / 4096.0
PQ_C2 = 2413.0 / 4096.0 * 32.0
PQ_C3 = 2392.0 / 4096.0 * 32.0

P0 = 1.0
P1 = 0.1
P3 = 0.5

M = [
    [0.4123907993, 0.3575843394, 0.1804807884],
    [0.2126390059, 0.7151686788, 0.0721923154],
    [0.0193308187, 0.1191947798, 0.9505321522],
]
MINV = [
    [3.240969896, -1.537383198, -0.498610765],
    [-0.969243646, 1.875967503, 0.041555058],
    [0.055630080, -0.203976959, 1.056971550],
]


def mat_vec(m, v):
    return [sum(m[i][j] * v[j] for j in range(3)) for i in range(3)]


def linear_to_pq(x: float, max_pq: float) -> float:
    normalized = (max(x, 0.0) / max(max_pq, 1e-6)) ** PQ_N
    nd = (PQ_C1 + PQ_C2 * normalized) / (1.0 + PQ_C3 * normalized)
    return nd**PQ_M


def pq_to_linear(x: float, max_pq: float) -> float:
    pq = max(x, 0.0) ** PQ_RCP_M
    nd = max(pq - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * pq)
    return (nd**PQ_RCP_N) * max_pq


def compute_pq_mul(y_in: float, y_out: float) -> float:
    pq_in = linear_to_pq(y_in, P0)
    pq_out = linear_to_pq(y_out * P3, P1)
    return pq_out / max(pq_in, 1e-6)


def pq_remap_y(y_in: float, y_out: float) -> float:
    pq_mul = compute_pq_mul(y_in, y_out)
    return pq_to_linear(linear_to_pq(y_in, P0) * pq_mul, P1) / P3


def apply_luma_only(rgb, y_in, y_out):
    xyz = mat_vec(M, rgb)
    f_luma = max(xyz[1], 0.0)
    if f_luma <= 0:
        return rgb
    pq_mul = compute_pq_mul(y_in, y_out)
    new_luma = pq_to_linear(linear_to_pq(f_luma, P0) * pq_mul, P1) / P3
    xyz_out = [c * (new_luma / f_luma) for c in xyz]
    return [max(c, 0.0) for c in mat_vec(MINV, xyz_out)]


def main() -> int:
    errors = 0

    # Monotonicity: higher y_out => higher remapped Y
    y_in = 40.0
    prev = 0.0
    for y_out in [50.0, 80.0, 120.0, 200.0, 400.0]:
        remapped = pq_remap_y(y_in, y_out)
        if remapped <= prev:
            print(f"FAIL monotonicity: y_out={y_out} remapped={remapped} prev={prev}")
            errors += 1
        prev = remapped

    # Target: remapped Y should approximate y_out
    for y_out in [80.0, 160.0, 320.0]:
        remapped = pq_remap_y(y_in, y_out)
        rel_err = abs(remapped - y_out) / y_out
        if rel_err > 0.05:
            print(f"FAIL target: y_out={y_out} remapped={remapped} err={rel_err:.3f}")
            errors += 1

    # No catastrophic negative RGB on saturated swatches
    swatches = [(72, 24, 24), (16, 40, 64), (76, 76, 76)]
    for rgb in swatches:
        out = apply_luma_only(rgb, 40.0, 160.0)
        if any(c < -0.01 for c in out):
            print(f"FAIL negative RGB: in={rgb} out={out}")
            errors += 1
        if out[0] == 0 and rgb[0] > 50:
            print(f"WARN red crushed: in={rgb} out={out}")

    if errors:
        print(f"{errors} check(s) failed")
        return 1

    print("OK: PQ sanity checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
