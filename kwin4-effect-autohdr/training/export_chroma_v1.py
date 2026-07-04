#!/usr/bin/env python3
"""Export chroma_v1.onnx from a trained PyTorch checkpoint (or copy v0 heuristic)."""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TRAINING = ROOT / "training"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=ROOT / "models" / "chroma_v1.onnx",
    )
    parser.add_argument(
        "--checkpoint",
        type=Path,
        default=TRAINING / "checkpoints" / "chroma_best.pt",
        help="PyTorch checkpoint from train_chroma.py",
    )
    parser.add_argument(
        "--fallback-v0",
        action="store_true",
        help="Copy chroma_v0.onnx when no checkpoint exists",
    )
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    if args.checkpoint.is_file():
        try:
            import torch

            sys.path.insert(0, str(TRAINING))
            from chroma_model import ChromaNet  # noqa: WPS433

            model = ChromaNet()
            state = torch.load(args.checkpoint, map_location="cpu", weights_only=True)
            model.load_state_dict(state)
            model.eval()

            dummy = torch.zeros(1, 3, 64, 64)
            torch.onnx.export(
                model,
                dummy,
                args.output,
                input_names=["input"],
                output_names=["output"],
                dynamic_axes={"input": {2: "H", 3: "W"}, "output": {2: "H", 3: "W"}},
                opset_version=17,
            )
            print(f"Exported trained model to {args.output}")
            return 0
        except ImportError as exc:
            print(f"PyTorch not available: {exc}", file=sys.stderr)

    v0 = ROOT / "models" / "chroma_v0.onnx"
    if args.fallback_v0 and v0.is_file():
        shutil.copy2(v0, args.output)
        print(f"Copied heuristic {v0} -> {args.output}")
        return 0

    print("No checkpoint found. Run tools/export_chroma_v0.py or train_chroma.py first.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
