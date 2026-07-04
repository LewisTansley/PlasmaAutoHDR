#!/usr/bin/env python3
"""Export a tiny guidance_v1.onnx model for AutoHDR plumbing tests.

Input:  float32 NCHW [1, 3, H, W] linear-relative RGB
Output: float32 NCHW [1, 4, H, W]:
  R = highlight expansion (>=1)
  G = highlight confidence / shoulder-detail mask [0,1]
  B = shadow detail mask [0,1]
  A = depth / local-contrast mask [0,1]

Per-pixel approximation of autohdr_guidance.frag (no neighborhood stats).
The default runtime path runs the full spatial formula on the GPU (onnx-style-gpu).
Keep highlight R/G in sync with the non-spatial terms in autohdr_guidance.frag:

  confidence     = saturate((luma - 0.55) / 0.40)
  highlight_lift = saturate((luma - 0.45) / 0.53)
  expansion      = 1 + confidence * highlight_lift * 0.75
  shadow_mask    = saturate(1 - luma / 0.14)
  depth_mask     = mid_region(luma)   # smoothstep band 0.12–0.88
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
        c("c_055", 0.55),
        c("c_040", 0.40),
        c("c_045", 0.45),
        c("c_053", 0.53),
        c("c_075", 0.75),
        c("c_014", 0.14),
        c("c_012", 0.12),
        c("c_022", 0.22),
        c("c_072", 0.72),
        c("c_088", 0.88),
        c("c_one", 1.0),
        c("c_zero", 0.0),
        # confidence = clip((luma - 0.55) / 0.40, 0, 1)
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
        # shadow_mask = clip(1 - luma / 0.14, 0, 1)
        helper.make_node("Div", ["luma", "c_014"], ["shadow_u"]),
        helper.make_node("Sub", ["c_one", "shadow_u"], ["shadow_raw"]),
        helper.make_node("Clip", ["shadow_raw", "c_zero", "c_one"], ["shadow_mask"]),
        # depth_mask = smoothstep(0.12,0.22,luma) * (1 - smoothstep(0.72,0.88,luma))
        # smoothstep(e0,e1,x) = t*t*(3-2*t), t=clip((x-e0)/(e1-e0),0,1)
        helper.make_node("Sub", ["luma", "c_012"], ["lo_num"]),
        helper.make_node("Sub", ["c_022", "c_012"], ["lo_den"]),
        helper.make_node("Div", ["lo_num", "lo_den"], ["lo_t_raw"]),
        helper.make_node("Clip", ["lo_t_raw", "c_zero", "c_one"], ["lo_t"]),
        helper.make_node("Mul", ["lo_t", "lo_t"], ["lo_t2"]),
        c("c_two", 2.0),
        c("c_three", 3.0),
        helper.make_node("Mul", ["c_two", "lo_t"], ["lo_2t"]),
        helper.make_node("Sub", ["c_three", "lo_2t"], ["lo_poly"]),
        helper.make_node("Mul", ["lo_t2", "lo_poly"], ["lo_s"]),
        helper.make_node("Sub", ["luma", "c_072"], ["hi_num"]),
        helper.make_node("Sub", ["c_088", "c_072"], ["hi_den"]),
        helper.make_node("Div", ["hi_num", "hi_den"], ["hi_t_raw"]),
        helper.make_node("Clip", ["hi_t_raw", "c_zero", "c_one"], ["hi_t"]),
        helper.make_node("Mul", ["hi_t", "hi_t"], ["hi_t2"]),
        helper.make_node("Mul", ["c_two", "hi_t"], ["hi_2t"]),
        helper.make_node("Sub", ["c_three", "hi_2t"], ["hi_poly"]),
        helper.make_node("Mul", ["hi_t2", "hi_poly"], ["hi_s"]),
        helper.make_node("Sub", ["c_one", "hi_s"], ["hi_keep"]),
        helper.make_node("Mul", ["lo_s", "hi_keep"], ["depth_mask"]),
        helper.make_node(
            "Concat",
            ["expansion", "confidence", "shadow_mask", "depth_mask"],
            ["output"],
            axis=1,
        ),
    ]

    graph = helper.make_graph(
        nodes,
        "autohdr_guidance_v1",
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
        default=Path(__file__).resolve().parents[1] / "models" / "guidance_v1.onnx",
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
