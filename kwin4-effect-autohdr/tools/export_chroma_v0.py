#!/usr/bin/env python3
"""Export a tiny chroma_v0.onnx model for AutoHDR chroma plumbing tests.

Input:  float32 NCHW [1, 3, H, W] linear-relative RGB
Output: float32 NCHW [1, 4, H, W]:
  R = chroma correction strength [0,1]
  G = saturation residual hint (0..1, remapped in shader)
  B = hue residual hint (0..1, remapped in shader)
  A = per-pixel colorIntensity blend [0,1]

Per-pixel approximation of autohdr_chroma_guidance.frag (no neighborhood stats).
The default runtime path runs the full spatial formula on the GPU.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np


def build_model():
    import onnx
    from onnx import TensorProto, helper, numpy_helper

    input_tensor = helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 3, "H", "W"])
    output_tensor = helper.make_tensor_value_info("output", TensorProto.FLOAT, [1, 4, "H", "W"])

    luma_w = numpy_helper.from_array(
        np.array([0.2126, 0.7152, 0.0722], dtype=np.float32).reshape(1, 3, 1, 1),
        name="luma_w",
    )
    axes = numpy_helper.from_array(np.array([1], dtype=np.int64), name="axes")

    def c(name: str, value: float):
        return helper.make_node(
            "Constant",
            [],
            [name],
            value=numpy_helper.from_array(np.array(value, dtype=np.float32)),
        )

    nodes = [
        helper.make_node("Mul", ["input", "luma_w"], ["weighted"]),
        helper.make_node("ReduceSum", ["weighted", "axes"], ["luma"], keepdims=1),
        c("c_008", 0.08),
        c("c_018", 0.18),
        c("c_082", 0.82),
        c("c_092", 0.92),
        c("c_055", 0.55),
        c("c_015", 0.15),
        c("c_half", 0.5),
        c("c_one", 1.0),
        c("c_zero", 0.0),
        # mid_region = smoothstep(0.08,0.18,l) * (1 - smoothstep(0.82,0.92,l))
        helper.make_node("Sub", ["luma", "c_008"], ["lo_num"]),
        helper.make_node("Sub", ["c_018", "c_008"], ["lo_den"]),
        helper.make_node("Div", ["lo_num", "lo_den"], ["lo_t_raw"]),
        helper.make_node("Clip", ["lo_t_raw", "c_zero", "c_one"], ["lo_t"]),
        helper.make_node("Mul", ["lo_t", "lo_t"], ["lo_t2"]),
        c("c_two", 2.0),
        c("c_three", 3.0),
        helper.make_node("Mul", ["c_two", "lo_t"], ["lo_2t"]),
        helper.make_node("Sub", ["c_three", "lo_2t"], ["lo_poly"]),
        helper.make_node("Mul", ["lo_t2", "lo_poly"], ["lo_s"]),
        helper.make_node("Sub", ["luma", "c_082"], ["hi_num"]),
        helper.make_node("Sub", ["c_092", "c_082"], ["hi_den"]),
        helper.make_node("Div", ["hi_num", "hi_den"], ["hi_t_raw"]),
        helper.make_node("Clip", ["hi_t_raw", "c_zero", "c_one"], ["hi_t"]),
        helper.make_node("Mul", ["hi_t", "hi_t"], ["hi_t2"]),
        helper.make_node("Mul", ["c_two", "hi_t"], ["hi_2t"]),
        helper.make_node("Sub", ["c_three", "hi_2t"], ["hi_poly"]),
        helper.make_node("Mul", ["hi_t2", "hi_poly"], ["hi_s"]),
        helper.make_node("Sub", ["c_one", "hi_s"], ["hi_keep"]),
        helper.make_node("Mul", ["lo_s", "hi_keep"], ["strength"]),
        # sat/hue hints neutral at 0.5
        helper.make_node("Concat", ["strength", "c_half", "c_half", "c_055"], ["output"], axis=1),
    ]

    graph = helper.make_graph(
        nodes,
        "autohdr_chroma_v0",
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
        default=Path(__file__).resolve().parents[1] / "models" / "chroma_v0.onnx",
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
