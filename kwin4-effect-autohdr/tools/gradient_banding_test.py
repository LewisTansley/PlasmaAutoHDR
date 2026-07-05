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


def make_shoulder_ramp(width: int, height: int, horizontal: bool = True) -> np.ndarray:
    """8-bit quantized ramp through highlight shoulder (luma ~0.82→0.98)."""
    t = np.linspace(0.82, 0.98, width if horizontal else height, dtype=np.float32)
    if horizontal:
        ramp = np.broadcast_to(t, (height, width))
    else:
        ramp = np.broadcast_to(t[:, None], (height, width))
    rgb = np.stack([ramp, ramp * 0.98, ramp * 0.95], axis=-1)
    srgb = linear_to_srgb(rgb)
    quantized = np.round(srgb * 255.0) / 255.0
    return srgb_to_linear(quantized).astype(np.float32)


def map_highlight_shoulder_rel(t: np.ndarray, expansion: float = 1.4,
                               confidence: float = 1.0, strength: float = 1.0) -> np.ndarray:
    """Mirror shader mapHighlightShoulderRel (single pre-curve shoulder)."""
    w = np.clip(confidence, 0.0, 1.0) * np.clip(strength, 0.0, 1.0)
    knee_lo, knee_hi = 0.92, 0.98
    knee_w = np.clip((t - knee_lo) / max(knee_hi - knee_lo, 1e-6), 0.0, 1.0)
    knee_w = knee_w * knee_w * (3.0 - 2.0 * knee_w)
    expansion = max(expansion, 1.0)
    rel_ceiling = 1.02 + (1.10 - 1.02) * np.clip((expansion - 1.0) / 0.75, 0.0, 1.0)
    u = np.clip((t - knee_lo) / max(1.0 - knee_lo, 1e-6), 0.0, 1.0)
    expanded = knee_lo + np.power(u, 0.85) * (rel_ceiling - knee_lo)
    new_t = np.maximum(np.where(w > 1e-4, t * (1.0 - knee_w * w) + expanded * (knee_w * w), t), t)
    return new_t.astype(np.float32)


def apply_highlight_shoulder_rgb(rgb: np.ndarray, ref: float = 203.0, expansion: float = 1.4,
                                 confidence: float = 1.0, strength: float = 1.0) -> np.ndarray:
    y = luma(rgb) * ref
    t = y / ref
    new_t = map_highlight_shoulder_rel(t, expansion, confidence, strength)
    return (rgb * (new_t / np.maximum(t, 1e-6))[..., None]).astype(np.float32)


def highlight_ramp_decontour(img: np.ndarray, ref: float = 203.0, strength: float = 0.5) -> np.ndarray:
    """1D row-wise highlight ramp decontour mirroring applyHighlightRampDecontour."""
    out = img.copy()
    h, w = out.shape[:2]
    lsb = 1.0 / 255.0
    kernel = np.array([0.05, 0.1, 0.15, 0.2, 0.2, 0.15, 0.1, 0.05], dtype=np.float64)
    for row in range(h):
        y = luma(out[row : row + 1, :, :]).flatten() * ref
        t = y / ref
        highlight_band = np.clip((t - 0.82) / 0.12, 0.0, 1.0) * (1.0 - np.clip((t - 0.97) / 0.05, 0.0, 1.0))
        d = np.abs(np.diff(y, prepend=y[0]))
        range_rel = np.max(d) / ref
        ramp_grad = np.clip((range_rel - 0.75 * lsb) / (0.25 * lsb), 0.0, 1.0)
        ramp_grad *= 1.0 - np.clip((range_rel - 1.75 * lsb) / (0.5 * lsb), 0.0, 1.0)
        decontour_w = ramp_grad * highlight_band * strength
        sm = np.convolve(y, kernel, mode="same")
        avg = sm
        dev = y - avg
        hint = np.sign(np.where(dev != 0.0, dev, 1.0)) * np.minimum(np.abs(dev), np.maximum(range_rel * ref * 0.45, ref * lsb))
        blend = 0.45 + 0.25 * highlight_band
        target_dev = dev * (1.0 - decontour_w * blend) + (dev + hint * 0.75) * (decontour_w * blend)
        new_y = np.maximum(avg + target_dev, 1e-6)
        scale = new_y / np.maximum(y, 1e-6)
        out[row, :, :] *= scale[:, None]
    return out.astype(np.float32)


def smoothstep(edge0: float, edge1: float, x: np.ndarray) -> np.ndarray:
    t = np.clip((x - edge0) / max(edge1 - edge0, 1e-6), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def apply_legacy_double_boost(rgb: np.ndarray, ref: float = 203.0, peak: float = 600.0,
                              ai_strength: float = 0.5) -> np.ndarray:
    """Legacy pre+post curve highlight boost (banding regression baseline)."""
    y = luma(rgb) * ref
    t = y / ref
    knee = 1.0 - 8.0 / 255.0
    u = np.clip((t - knee) / max(1.0 - knee, 1e-6), 0.0, 1.0)
    expanded = knee + np.power(u, 0.75) * (1.05 - knee)
    knee_w = smoothstep(knee, knee + (8.0 / 255.0) * 0.5, t)
    new_t = np.maximum(t * (1.0 - knee_w * ai_strength) + expanded * (knee_w * ai_strength), t)
    expanded_rgb = rgb * (new_t / np.maximum(t, 1e-6))[..., None]
    toned = apply_tone_curve(expanded_rgb, ref, peak)
    y2 = luma(toned) * ref
    boosted = np.minimum(y2 * 1.4, peak)
    scale = (y2 * (1.0 - ai_strength) + boosted * ai_strength) / np.maximum(y2, 1e-6)
    return (toned * scale[..., None]).astype(np.float32)


def validate_shoulder_map() -> bool:
    """Unified shoulder curve must stay monotonic with bounded pre-curve ceiling."""
    t = np.linspace(0.5, 1.0, 256, dtype=np.float32)
    for expansion in (1.0, 1.25, 1.4, 1.75):
        for strength in (0.5, 1.0):
            mapped = map_highlight_shoulder_rel(t, expansion, 1.0, strength)
            if np.any(np.diff(mapped) < -1e-5):
                return False
            if np.max(mapped) > 1.12:
                return False
            d2 = np.diff(mapped, 2)
            if float(np.max(np.abs(d2))) > 0.02:
                return False
    return True


def apply_ai_shoulder_pipeline(rgb: np.ndarray, ref: float = 203.0, peak: float = 600.0,
                               ai_strength: float = 0.5) -> np.ndarray:
    """Pre+post curve path with unified shoulder (no double boost)."""
    decontoured = highlight_ramp_decontour(rgb, ref, ai_strength)
    expanded = apply_highlight_shoulder_rgb(decontoured, ref, 1.4, 1.0, ai_strength)
    return apply_tone_curve(expanded, ref, peak)


def banding_metric_shoulder(img: np.ndarray) -> float:
    """Banding metric on full rows (shoulder ramp images span the highlight band only)."""
    y = luma(img)
    scores = [banding_metric_1d(row.astype(np.float64)) for row in y]
    return float(np.mean(scores)) if scores else 0.0


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

    shoulder = make_shoulder_ramp(args.width, max(32, args.height // 4))
    shoulder_toned = apply_tone_curve(shoulder)
    shoulder_legacy = apply_legacy_double_boost(shoulder)
    shoulder_new = apply_tone_curve(apply_highlight_shoulder_rgb(shoulder, strength=0.5))
    shoulder_map_ok = validate_shoulder_map()

    shoulder_legacy_score = banding_metric_shoulder(shoulder_legacy)
    shoulder_new_score = banding_metric_shoulder(shoulder_new)

    save_png(args.output_dir / "dual_bar_tonemapped.png", tonemapped)
    save_png(args.output_dir / "dual_bar_debanded.png", debanded)
    save_png(args.output_dir / "shoulder_ramp_tonemapped.png", shoulder_toned)
    save_png(args.output_dir / "shoulder_ramp_legacy_ai.png", shoulder_legacy)
    save_png(args.output_dir / "shoulder_ramp_unified_ai.png", shoulder_new)

    print(f"Banding metric (tonemapped): {raw_score:.6e}")
    print(f"Banding metric (debanded):    {deband_score:.6e}")
    print(f"Reduction:                    {reduction:.1f}%")
    print(f"Shoulder map validation:        {'PASS' if shoulder_map_ok else 'FAIL'}")
    print(f"Shoulder metric (legacy AI):    {shoulder_legacy_score:.6e}")
    print(f"Shoulder metric (unified AI):   {shoulder_new_score:.6e}")
    print(f"Wrote PNGs to {args.output_dir}")

    failed = False
    if reduction < args.min_reduction:
        print(f"FAIL: reduction {reduction:.1f}% < {args.min_reduction}% threshold", file=sys.stderr)
        failed = True
    if not shoulder_map_ok:
        print("FAIL: unified shoulder map is non-monotonic or exceeds pre-curve ceiling", file=sys.stderr)
        failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
