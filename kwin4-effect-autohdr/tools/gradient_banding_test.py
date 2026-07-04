#!/usr/bin/env python3
"""Synthetic gradient banding metric and visual regression for AutoHDR AI debanding.

Generates dual 8-bit quantized linear ramps (matching the screenshot test pattern),
applies a tone curve, then band-weighted post-curve spatial deband.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

LUMA_W = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def srgb_to_linear(c: np.ndarray) -> np.ndarray:
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c: np.ndarray) -> np.ndarray:
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * (c ** (1.0 / 2.4)) - 0.055)


def luma(rgb: np.ndarray) -> np.ndarray:
    return np.tensordot(rgb, LUMA_W, axes=([-1], [0]))


def make_ramp(width: int, height: int, horizontal: bool = True, invert: bool = False) -> np.ndarray:
    t = np.linspace(0.0, 1.0, width if horizontal else height, dtype=np.float32)
    if invert:
        t = 1.0 - t
    if horizontal:
        ramp = np.broadcast_to(t, (height, width))
    else:
        ramp = np.broadcast_to(t[:, None], (height, width))
    rgb = np.stack([ramp, ramp, ramp * 0.95], axis=-1)
    srgb = linear_to_srgb(rgb)
    quantized = np.round(srgb * 255.0) / 255.0
    return srgb_to_linear(quantized).astype(np.float32)


def make_dual_bar(width: int, height: int) -> np.ndarray:
    """Top: black→white, bottom: white→black (screenshot test pattern)."""
    half = height // 2
    top = make_ramp(width, half, horizontal=True, invert=False)
    bottom = make_ramp(width, height - half, horizontal=True, invert=True)
    return np.concatenate([top, bottom], axis=0)


def balanced_tone_curve(t: np.ndarray, ref: float = 203.0, peak: float = 600.0) -> np.ndarray:
    u = np.clip(t, 0.0, 1.0)
    toe = np.power(u, 0.85)
    shoulder = 1.0 - np.power(1.0 - u, 1.15)
    mixed = np.where(u < 0.5, toe * 0.5 + u * 0.5, u * 0.5 + shoulder * 0.5)
    return mixed * peak


def apply_tone_curve(rgb: np.ndarray, ref: float = 203.0, peak: float = 600.0) -> np.ndarray:
    y = luma(rgb) * ref
    t = y / ref
    out_y = balanced_tone_curve(t, ref, peak)
    scale = out_y / np.maximum(y, 1e-6)
    return rgb * scale[..., None]


def estimate_band_mask(y_rel: np.ndarray) -> np.ndarray:
    """Per-pixel midtone band mask (luma-only approximation of guidance channel A)."""
    mid = np.clip((y_rel - 0.05) / 0.10, 0.0, 1.0) * (1.0 - np.clip((y_rel - 0.70) / 0.18, 0.0, 1.0))
    shadow = (1.0 - np.clip(y_rel / 0.12, 0.0, 1.0))
    shoulder = np.clip((y_rel - 0.82) / 0.12, 0.0, 1.0) * (1.0 - np.clip((y_rel - 0.94) / 0.06, 0.0, 1.0))
    return np.clip(np.maximum(mid, np.maximum(shadow * 0.6, shoulder * 0.5)), 0.0, 1.0)


def post_curve_deband(img: np.ndarray, ref: float = 203.0, spatial_strength: float = 0.55,
                      band_strength: float = 0.7, ai_strength: float = 0.5) -> np.ndarray:
    """Reference post-curve deband mirroring fixed shader logic (band-weighted spatial avg)."""
    out = img.copy()
    h, w = out.shape[:2]
    y = luma(out) * ref
    y_rel = y / ref
    band = estimate_band_mask(y_rel)
    band_w = band * band_strength * ai_strength
    kernel = np.array([0.05, 0.1, 0.15, 0.2, 0.2, 0.15, 0.1, 0.05], dtype=np.float64)

    for row in range(h):
        profile = y[row, :].astype(np.float64)
        row_band = band_w[row, :].astype(np.float64)
        band_boost = 1.0 + 0.55 * row_band
        deband_w = spatial_strength * band_boost

        sm = np.convolve(profile, kernel, mode="same")
        blend = np.clip(deband_w * (0.35 + 0.65 * row_band), 0.0, 0.92)
        scale = sm / np.maximum(profile, 1e-6)
        out[row, :, :] *= (1.0 - blend[:, None] + blend[:, None] * scale[:, None])

    return out.astype(np.float32)


def banding_metric_1d(profile: np.ndarray) -> float:
    if profile.size < 4:
        return 0.0
    d1 = np.diff(profile)
    d2 = np.diff(d1)
    return float(np.var(d2))


def banding_metric_image(img: np.ndarray, horizontal: bool = True) -> float:
    y = luma(img)
    profiles = y if horizontal else y.T
    scores = [banding_metric_1d(row.astype(np.float64)) for row in profiles]
    return float(np.mean(scores))


def save_png(path: Path, rgb_linear: np.ndarray) -> None:
    try:
        from PIL import Image
    except ImportError:
        print("PIL not installed; skipping PNG output")
        return
    srgb = linear_to_srgb(np.clip(rgb_linear, 0.0, 1.0))
    img = (srgb * 255.0 + 0.5).astype(np.uint8)
    Image.fromarray(img, mode="RGB").save(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output-dir", type=Path, default=Path("banding_test_out"))
    parser.add_argument("--width", type=int, default=512)
    parser.add_argument("--height", type=int, default=256)
    parser.add_argument("--min-reduction", type=float, default=15.0,
                        help="Minimum banding metric reduction percent (default 15)")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    ramp = make_dual_bar(args.width, args.height)
    tonemapped = apply_tone_curve(ramp)
    debanded = post_curve_deband(tonemapped)

    raw_score = banding_metric_image(tonemapped)
    deband_score = banding_metric_image(debanded)
    reduction = (1.0 - deband_score / max(raw_score, 1e-12)) * 100.0

    save_png(args.output_dir / "dual_bar_tonemapped.png", tonemapped)
    save_png(args.output_dir / "dual_bar_debanded.png", debanded)

    print(f"Banding metric (tonemapped): {raw_score:.6e}")
    print(f"Banding metric (debanded):    {deband_score:.6e}")
    print(f"Reduction:                    {reduction:.1f}%")
    print(f"Wrote PNGs to {args.output_dir}")

    if reduction < args.min_reduction:
        print(f"FAIL: reduction {reduction:.1f}% < {args.min_reduction}% threshold", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
