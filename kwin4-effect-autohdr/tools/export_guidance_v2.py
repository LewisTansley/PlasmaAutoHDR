#!/usr/bin/env python3
"""Train and validate guidance_v2.onnx, then run ORT ramp sanity checks."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np


def ort_ramp_sanity(model_path: Path, size: int = 128) -> tuple[float, float]:
    import onnxruntime as ort

    sess = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    name = sess.get_inputs()[0].name

    t = np.linspace(0.05, 0.92, size, dtype=np.float32)
    ramp = np.stack(
        [np.broadcast_to(t, (size, size)), np.broadcast_to(t * 0.98, (size, size)), np.broadcast_to(t * 0.95, (size, size))]
    )[None]
    flat = np.full((1, 3, size, size), 0.5, dtype=np.float32)

    ramp_a = sess.run(None, {name: ramp})[0][0, 3]
    flat_a = sess.run(None, {name: flat})[0][0, 3]
    return float(ramp_a.std()), float(flat_a.mean())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "models" / "guidance_v2.onnx",
    )
    parser.add_argument("--epochs", type=int, default=80)
    parser.add_argument("--size", type=int, default=128)
    parser.add_argument("--min-ramp-std", type=float, default=0.05)
    args = parser.parse_args()

    train_script = Path(__file__).resolve().parent / "train_guidance_v2.py"
    cmd = [
        sys.executable,
        str(train_script),
        "-o",
        str(args.output),
        "--epochs",
        str(args.epochs),
        "--size",
        str(args.size),
        "--min-ramp-std",
        str(args.min_ramp_std),
    ]
    rc = subprocess.call(cmd)
    if rc != 0:
        return rc

    ramp_std, flat_mean = ort_ramp_sanity(args.output, args.size)
    print(f"ORT sanity: ramp A std={ramp_std:.4f}, flat A mean={flat_mean:.4f}")
    if ramp_std < args.min_ramp_std:
        print(f"FAIL: ramp std {ramp_std:.4f} < {args.min_ramp_std}", file=sys.stderr)
        return 1
    if flat_mean > 0.15:
        print(f"FAIL: flat mean {flat_mean:.4f} > 0.15", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
