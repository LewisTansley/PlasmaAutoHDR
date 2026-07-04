#!/usr/bin/env python3
"""Train guidance_v2.onnx — spatial banding mask + distilled highlight/shadow channels."""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np

LUMA_W = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def smoothstep(edge0: float, edge1: float, x: np.ndarray) -> np.ndarray:
    t = np.clip((x - edge0) / max(edge1 - edge0, 1e-6), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def luma(rgb: np.ndarray) -> np.ndarray:
    return np.tensordot(rgb, LUMA_W, axes=([-1], [0]))


def srgb_to_linear(c: np.ndarray) -> np.ndarray:
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c: np.ndarray) -> np.ndarray:
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * (c ** (1.0 / 2.4)) - 0.055)


def quantize_8bit(rgb: np.ndarray) -> np.ndarray:
    srgb = linear_to_srgb(rgb)
    return srgb_to_linear(np.round(srgb * 255.0) / 255.0)


def tone_curve(t: np.ndarray, mode: str) -> np.ndarray:
    u = np.clip(t, 0.0, 1.0)
    if mode == "lifted":
        return np.power(u, 0.82)
    if mode == "contrast":
        return np.clip((u - 0.5) * 1.35 + 0.5, 0.0, 1.0)
    return np.where(u < 0.5, np.power(u, 0.9) * 0.5 + u * 0.5, u * 0.5 + (1.0 - np.power(1.0 - u, 1.1)) * 0.5)


def apply_curve_rgb(rgb: np.ndarray, ref: float, peak: float, mode: str) -> np.ndarray:
    y = luma(rgb) * ref
    t = y / ref
    out = tone_curve(t, mode) * peak
    return rgb * (out / np.maximum(y, 1e-6))[..., None]


def banding_score_map(curved: np.ndarray, ref: float = 203.0) -> np.ndarray:
    y = luma(curved) / ref
    h, w = y.shape
    score = np.zeros((h, w), dtype=np.float32)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dx == 0 and dy == 0:
                continue
            shifted = np.roll(np.roll(y, dy, axis=0), dx, axis=1)
            score += np.abs(y - shifted)
    score /= 8.0
    mid = smoothstep(0.08, 0.18, y) * (1.0 - smoothstep(0.65, 0.82, y))
    return np.clip(score * 8.0 * mid, 0.0, 1.0).astype(np.float32)


def teacher_v15(rgb: np.ndarray) -> np.ndarray:
    y = luma(rgb)
    conf = np.clip((y - 0.55) / 0.40, 0.0, 1.0)
    lift = np.clip((y - 0.45) / 0.53, 0.0, 1.0)
    expansion = 1.0 + conf * lift * 0.75
    shadow = np.clip(1.0 - y / 0.14, 0.0, 1.0)
    return np.stack([expansion, conf, shadow], axis=0).astype(np.float32)


def make_ramp(h: int, w: int, horizontal: bool) -> np.ndarray:
    t = np.linspace(0.05, 0.92, w if horizontal else h, dtype=np.float32)
    if horizontal:
        base = np.broadcast_to(t, (h, w))
    else:
        base = np.broadcast_to(t[:, None], (h, w))
    rgb = np.stack([base, base * 0.98, base * 0.95], axis=-1)
    return quantize_8bit(rgb)


def make_radial_ramp(h: int, w: int) -> np.ndarray:
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    cx, cy = (w - 1) * 0.5, (h - 1) * 0.5
    r = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
    t = np.clip(r / max(r.max(), 1e-6), 0.05, 0.92)
    rgb = np.stack([t, t * 0.97, t * 0.94], axis=-1)
    return quantize_8bit(rgb)


def make_multistop_ramp(h: int, w: int) -> np.ndarray:
    t = np.linspace(0.0, 1.0, w, dtype=np.float32)
    stops = np.array([0.05, 0.25, 0.55, 0.82], dtype=np.float32)
    idx = np.searchsorted(stops, t, side="right") - 1
    idx = np.clip(idx, 0, len(stops) - 2)
    local = (t - stops[idx]) / np.maximum(stops[idx + 1] - stops[idx], 1e-6)
    v = stops[idx] + local * (stops[idx + 1] - stops[idx])
    base = np.broadcast_to(v, (h, w))
    rgb = np.stack([base, base * 0.98, base * 0.96], axis=-1)
    return quantize_8bit(rgb)


def make_noise(h: int, w: int, rng: np.random.Generator) -> np.ndarray:
    return quantize_8bit(rng.random((h, w, 3), dtype=np.float32))


def make_text_like(h: int, w: int) -> np.ndarray:
    img = np.full((h, w, 3), 0.15, dtype=np.float32)
    for row in range(0, h, 14):
        img[row : row + 2, :] = 0.85
    return quantize_8bit(img)


def generate_batch(size: int, rng: np.random.Generator) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    h = w = size
    inputs = []
    targets = []
    is_ramp = []
    modes = ["balanced", "lifted", "contrast"]
    generators = [
        ("ramp_h", lambda: make_ramp(h, w, True)),
        ("ramp_v", lambda: make_ramp(h, w, False)),
        ("radial", lambda: make_radial_ramp(h, w)),
        ("multistop", lambda: make_multistop_ramp(h, w)),
        ("noise", lambda: make_noise(h, w, rng)),
        ("text", lambda: make_text_like(h, w)),
    ]
    for _name, gen in generators:
        for mode in modes:
            rgb = gen()
            curved = apply_curve_rgb(rgb, 203.0, 600.0, mode)
            band = banding_score_map(curved)
            teacher = teacher_v15(rgb)
            target = np.concatenate([teacher, band[None, ...]], axis=0)
            inputs.append(rgb.transpose(2, 0, 1))
            targets.append(target)
            is_ramp.append(_name.startswith("ramp") or _name in {"radial", "multistop"})
    ramp_mask = np.array(is_ramp, dtype=np.bool_)
    return np.stack(inputs), np.stack(targets), ramp_mask


def validate_ramp_spatial(model, size: int = 128) -> tuple[float, float]:
    import torch

    ramp = make_ramp(size, size, True).transpose(2, 0, 1)[None]
    flat = np.full((1, 3, size, size), 0.5, dtype=np.float32)
    with torch.no_grad():
        ramp_a = model(torch.from_numpy(ramp))[0, 3].numpy()
        flat_a = model(torch.from_numpy(flat))[0, 3].numpy()
    return float(ramp_a.std()), float(flat_a.mean())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", type=int, default=128)
    parser.add_argument("--epochs", type=int, default=80)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--min-ramp-std", type=float, default=0.05)
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "models" / "guidance_v2.onnx",
    )
    args = parser.parse_args()

    try:
        import torch
        import torch.nn as nn
        import torch.nn.functional as F
    except ImportError as exc:
        raise SystemExit("PyTorch required: pip install torch") from exc

    class DepthwiseSeparable(nn.Module):
        def __init__(self, ch: int, hidden: int):
            super().__init__()
            self.dw = nn.Conv2d(ch, ch, 5, padding=2, groups=ch, bias=False)
            self.pw = nn.Conv2d(ch, hidden, 1, bias=False)
            self.act = nn.ReLU(inplace=True)

        def forward(self, x):
            return self.act(self.pw(self.dw(x)))

    class GuidanceV2(nn.Module):
        def __init__(self):
            super().__init__()
            self.stem = nn.Conv2d(3, 16, 3, padding=1, bias=False)
            self.block1 = DepthwiseSeparable(16, 24)
            self.block2 = DepthwiseSeparable(24, 32)
            self.block3 = DepthwiseSeparable(32, 32)
            self.head = nn.Conv2d(32, 4, 1)

        def forward(self, x):
            x = F.relu(self.stem(x))
            x = self.block1(x)
            x = self.block2(x)
            x = self.block3(x)
            out = self.head(x)
            out_r = torch.clamp(out[:, 0:1], min=1.0)
            out_g = torch.sigmoid(out[:, 1:2])
            out_b = torch.sigmoid(out[:, 2:3])
            out_a = torch.sigmoid(out[:, 3:4])
            return torch.cat([out_r, out_g, out_b, out_a], dim=1)

    rng = np.random.default_rng(42)

    model = GuidanceV2()
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)

    best_ramp_std = -1.0
    best_state = None

    for epoch in range(args.epochs):
        x_np, y_np, ramp_mask_np = generate_batch(args.size, rng)
        x = torch.from_numpy(x_np)
        y = torch.from_numpy(y_np)
        ramp_mask = torch.from_numpy(ramp_mask_np)

        opt.zero_grad()
        pred = model(x)
        loss_distill = F.mse_loss(pred[:, :3], y[:, :3])
        loss_band = F.binary_cross_entropy(pred[:, 3:4], y[:, 3:4])
        tv = (
            torch.mean(torch.abs(pred[:, 3:4, :, 1:] - pred[:, 3:4, :, :-1]))
            + torch.mean(torch.abs(pred[:, 3:4, 1:, :] - pred[:, 3:4, :-1, :]))
        )
        ramp_a = pred[ramp_mask, 3:4]
        ramp_std_batch = torch.std(ramp_a, dim=(1, 2, 3))
        spatial_penalty = torch.relu(0.12 - ramp_std_batch).mean()
        loss = loss_distill + 2.5 * loss_band + 0.02 * tv + 3.0 * spatial_penalty
        loss.backward()
        opt.step()

        ramp_std, flat_mean = validate_ramp_spatial(model, args.size)
        if ramp_std > best_ramp_std:
            best_ramp_std = ramp_std
            best_state = {k: v.detach().clone() for k, v in model.state_dict().items()}

        if (epoch + 1) % 10 == 0 or epoch == 0:
            print(
                f"epoch {epoch + 1}/{args.epochs} loss={loss.item():.4f} "
                f"band={loss_band.item():.4f} ramp_std={ramp_std:.4f} flat_mean={flat_mean:.4f} "
                f"best={best_ramp_std:.4f}"
            )

    if best_state is not None:
        model.load_state_dict(best_state)

    ramp_std, flat_mean = validate_ramp_spatial(model, args.size)
    if ramp_std < args.min_ramp_std:
        raise SystemExit(
            f"Export rejected: ramp bandMask std {ramp_std:.4f} < {args.min_ramp_std} (model collapsed to flat output)"
        )
    if flat_mean > 0.15:
        raise SystemExit(f"Export rejected: flat bandMask mean {flat_mean:.4f} > 0.15")

    model.eval()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros(1, 3, args.size, args.size)
    torch.onnx.export(
        model,
        dummy,
        args.output,
        input_names=["input"],
        output_names=["output"],
        dynamic_axes={"input": {2: "H", 3: "W"}, "output": {2: "H", 3: "W"}},
        opset_version=13,
    )
    print(f"Wrote {args.output} (ramp_std={ramp_std:.4f}, flat_mean={flat_mean:.4f})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
