"""Shared AutoHDR configuration schema for Decky backend and frontend."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass
class FieldSpec:
    key: str
    label: str
    field_type: str
    minimum: float | None = None
    maximum: float | None = None
    step: float | None = None
    options: list[str] | None = None


CALIBRATION_FIELDS: list[FieldSpec] = [
    FieldSpec("ReferenceNits", "Reference Nits", "int", 80, 480, 1),
    FieldSpec("MaxNits", "Max Nits", "float", 200, 4000, 10),
    FieldSpec("GamutExpansion", "Gamut Expansion", "float", 0, 20, 0.1),
    FieldSpec("Vibrance", "Vibrance", "float", 0, 10, 0.1),
    FieldSpec("BlackPoint", "Black Point", "float", -0.01, 0.01, 0.001),
    FieldSpec(
        "ToneCurvePreset",
        "Tone Curve Preset",
        "enum",
        options=[
            "linear",
            "scurve",
            "scurve_boosted",
            "scurve_lifted",
            "exponential",
            "custom",
            "user",
        ],
    ),
]

CONFIG_PATH = "~/.config/kwin4effectautohdr"
WRAPPER_PATH = "~/autohdr"
LAUNCH_OPTION = "~/autohdr %command%"


def field_defaults() -> dict[str, Any]:
    return {
        "ReferenceNits": 203,
        "MaxNits": 1000.0,
        "GamutExpansion": 1.5,
        "Vibrance": 0.0,
        "BlackPoint": 0.0,
        "ToneCurvePreset": "scurve",
        "ToneCurveUserPresetId": "",
    }
