#!/usr/bin/env python3
"""Train ChromaNet on synthetic 8-bit chroma degradation pairs."""

from __future__ import annotations

import argparse
from pathlib import Path

import torch
import torch.nn.functional as F
from torch.utils.data import DataLoader, Dataset

from chroma_model import ChromaNet, y_lock_loss


class SyntheticChromaDataset(Dataset):
    def __init__(self, count: int = 512, size: int = 128):
        self.count = count
        self.size = size

    def __len__(self) -> int:
        return self.count

    def __getitem__(self, idx: int):
        g = torch.Generator().manual_seed(idx + 42)
        h = w = self.size
        # Smooth HDR-ish gradient ground truth
        u = torch.linspace(0, 1, w, generator=g).view(1, 1, w).expand(3, h, w)
        v = torch.linspace(0, 1, h, generator=g).view(1, h, 1).expand(3, h, w)
        hue = torch.rand(3, 1, 1, generator=g) * 0.6 + 0.2
        gt = (u * hue).clamp(0, 1)
        gt = gt * (0.3 + 0.7 * v)

        # 8-bit quantize + chroma banding
        degraded = (gt * 255.0).round() / 255.0
        step = torch.rand(1, generator=g).item() * 0.08 + 0.02
        degraded = (degraded / step).floor() * step

        target_map = torch.zeros(4, h, w)
        target_map[0] = 0.7  # strength
        target_map[1] = 0.5  # neutral sat hint
        target_map[2] = 0.5  # neutral hue hint
        target_map[3] = 0.35  # colorIntensity
        return degraded, gt, target_map


def apply_chroma_map(rgb: torch.Tensor, chroma_map: torch.Tensor) -> torch.Tensor:
    """Differentiable Y-lock chroma nudge for training."""
    w = chroma_map[:, 0:1]
    sat_delta = (chroma_map[:, 1:2] - 0.5) * 0.2
    mean = rgb.mean(dim=1, keepdim=True)
    centered = rgb - mean
    scaled = mean + centered * (1.0 + sat_delta * w)
    y0 = (rgb * rgb.new_tensor([0.2126, 0.7152, 0.0722]).view(1, 3, 1, 1)).sum(1, keepdim=True)
    y1 = (scaled * rgb.new_tensor([0.2126, 0.7152, 0.0722]).view(1, 3, 1, 1)).sum(1, keepdim=True)
    return scaled * (y0 / y1.clamp_min(1e-6))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--epochs", type=int, default=5)
    parser.add_argument("--batch", type=int, default=8)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument(
        "--out",
        type=Path,
        default=Path(__file__).resolve().parent / "checkpoints" / "chroma_best.pt",
    )
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    model = ChromaNet().to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr)
    loader = DataLoader(SyntheticChromaDataset(), batch_size=args.batch, shuffle=True)

    best = float("inf")
    for epoch in range(args.epochs):
        total = 0.0
        for degraded, gt, target_map in loader:
            degraded = degraded.to(device)
            gt = gt.to(device)
            target_map = target_map.to(device)

            pred_map = model(degraded)
            refined = apply_chroma_map(degraded, pred_map)

            loss_map = F.l1_loss(pred_map, target_map)
            loss_rgb = F.l1_loss(refined, gt)
            loss_y = y_lock_loss(degraded, refined)
            loss = loss_map + loss_rgb + 2.0 * loss_y

            opt.zero_grad()
            loss.backward()
            opt.step()
            total += loss.item()

        avg = total / len(loader)
        print(f"epoch {epoch + 1}/{args.epochs} loss={avg:.4f}")
        if avg < best:
            best = avg
            torch.save(model.state_dict(), args.out)
            print(f"  saved {args.out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
