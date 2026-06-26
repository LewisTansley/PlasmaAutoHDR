#pragma once

#include "types.h"

#include <string>
#include <vector>

namespace AutoHdrCore {

struct PresetCurveParams {
    float referenceNits = 203.0f;
    float peakNits = 1000.0f;
};

std::string presetToString(ToneCurvePreset preset);
ToneCurvePreset presetFromString(const std::string &encoded);
std::vector<ToneCurvePreset> builtInToneCurvePresets();
std::string presetDisplayName(ToneCurvePreset preset);

PointList generatePresetIntermediatePoints(ToneCurvePreset preset, const PresetCurveParams &params);
void applyToneCurvePreset(CalibrationSettings &settings,
                          const std::vector<UserToneCurvePreset> *userPresets = nullptr);
ToneCurvePreset detectMatchingPreset(const CalibrationSettings &settings, float toleranceNits,
                                     const std::vector<UserToneCurvePreset> *userPresets = nullptr);

} // namespace AutoHdrCoreCore
