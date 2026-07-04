#!/usr/bin/env python3
"""Evaluate chroma delta-E (ab* proxy) with Y locked."""

from __future__ import annotations

import argparse

import torch

from chroma_model import ChromaNet


def rgb_to_lab_ab(rgb: torch.Tensor) -> torch.Tensor:
    """Simplified ab* proxy for chroma-only error."""
    r, g, b = rgb[:, 0], rgb[:, 1], rgb[:, 2]
    a = r - g
    b_ = (r + g) * 0.5 - b
    return torch.stack([a, b_], dim=1)


def delta_e_chroma(ref: torch.Tensor, test: torch.Tensor) -> float:
    ab_ref = rgb_to_lab_ab(ref)
    ab_test = rgb_to_lab_ab(test)
    return torch.sqrt(((ab_ref - ab_test) ** 2).sum(dim=1).mean()).item()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", type=str, default="checkpoints/chroma_best.pt")
    args = parser.parse_args()

    model = ChromaNet()
    try:
        state = torch.load(args.checkpoint, map_location="cpu", weights_only=True)
        model.load_state_dict(state)
    except FileNotFoundError:
        print("No checkpoint; run train_chroma.py first")
        return 1

    model.eval()
    x = torch.rand(1, 3, 64, 64)
    with torch.no_grad():
        m = model(x)
    print(f"Output shape {tuple(m.shape)}, mean strength {m[:, 0].mean():.3f}")
    print("eval_delta_e: placeholder OK (train for meaningful metrics)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
