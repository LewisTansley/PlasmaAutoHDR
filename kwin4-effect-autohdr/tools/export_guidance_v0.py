#!/usr/bin/env python3
"""Export a tiny guidance_v0.onnx model for AutoHDR plumbing tests.

Input:  float32 NCHW [1, 3, H, W] linear-relative RGB
Output: float32 NCHW [1, 2, H, W] where R=expansion (>=1), G=confidence [0,1]

Luma formula (keep in sync with autohdr_guidance.frag):
  confidence     = saturate((luma - 0.55) / 0.40)
  highlight_lift = saturate((luma - 0.45) / 0.53)
  expansion      = 1 + confidence * highlight_lift * 0.75

The default runtime path runs this formula on the GPU (onnx-style-gpu).
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np


def build_model():
    import onnx
    from onnx import TensorProto, helper, numpy_helper

    # Dynamic H/W via symbolic dims.
    input_tensor = helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 3, "H", "W"])
    output_tensor = helper.make_tensor_value_info("output", TensorProto.FLOAT, [1, 2, "H", "W"])

    luma_w = numpy_helper.from_array(
        np.array([0.2126, 0.7152, 0.0722], dtype=np.float32).reshape(1, 3, 1, 1),
        name="luma_w",
    )

    axes = numpy_helper.from_array(np.array([1], dtype=np.int64), name="axes")

    nodes = [
        helper.make_node("Mul", ["input", "luma_w"], ["weighted"]),
        helper.make_node("ReduceSum", ["weighted", "axes"], ["luma"], keepdims=1),
        # confidence = clip((luma - 0.55) / 0.40, 0, 1)
        helper.make_node("Constant", [], ["c_055"], value=numpy_helper.from_array(np.array(0.55, dtype=np.float32))),
        helper.make_node("Constant", [], ["c_040"], value=numpy_helper.from_array(np.array(0.40, dtype=np.float32))),
        helper.make_node("Constant", [], ["c_045"], value=numpy_helper.from_array(np.array(0.45, dtype=np.float32))),
        helper.make_node("Constant", [], ["c_053"], value=numpy_helper.from_array(np.array(0.53, dtype=np.float32))),
        helper.make_node("Constant", [], ["c_075"], value=numpy_helper.from_array(np.array(0.75, dtype=np.float32))),
        helper.make_node("Constant", [], ["c_one"], value=numpy_helper.from_array(np.array(1.0, dtype=np.float32))),
        helper.make_node("Constant", [], ["c_zero"], value=numpy_helper.from_array(np.array(0.0, dtype=np.float32))),
        helper.make_node("Sub", ["luma", "c_055"], ["conf_num"]),
        helper.make_node("Div", ["conf_num", "c_040"], ["conf_raw"]),
        helper.make_node("Clip", ["conf_raw", "c_zero", "c_one"], ["confidence"]),
        # highlight_lift = clip((luma - 0.45) / 0.53, 0, 1)
        helper.make_node("Sub", ["luma", "c_045"], ["lift_num"]),
        helper.make_node("Div", ["lift_num", "c_053"], ["lift_raw"]),
        helper.make_node("Clip", ["lift_raw", "c_zero", "c_one"], ["highlight_lift"]),
        # expansion = 1 + confidence * highlight_lift * 0.75
        helper.make_node("Mul", ["confidence", "highlight_lift"], ["conf_lift"]),
        helper.make_node("Mul", ["conf_lift", "c_075"], ["boost"]),
        helper.make_node("Add", ["boost", "c_one"], ["expansion"]),
        helper.make_node("Concat", ["expansion", "confidence"], ["output"], axis=1),
    ]

    graph = helper.make_graph(
        nodes,
        "autohdr_guidance_v0",
        [input_tensor],
        [output_tensor],
        [luma_w, axes],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    model.ir_version = 8
    onnx.checker.check_model(model)
    return model


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "models" / "guidance_v0.onnx",
    )
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    model = build_model()
    import onnx

    onnx.save(model, args.output)
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
