#pragma once

#include "types.h"

#include <optional>
#include <string>

namespace AutoHdrCore {

std::string userPresetComboId(const std::string &id);
bool isUserPresetComboId(const std::string &comboId);
std::string userPresetIdFromComboId(const std::string &comboId);
std::string sanitizeUserPresetKey(const std::string &raw);

std::string formatNormalizedPoints(const PointList &points);
PointList parseNormalizedPoints(const std::string &encoded);
std::string formatSdrMaxFraction(const Vec2 &fraction);
Vec2 parseSdrMaxFraction(const std::string &encoded, const Vec2 &fallback = {1.0f, 1.0f});

UserToneCurvePreset normalizeCurrentCurve(const CalibrationSettings &settings);
void applyUserToneCurvePreset(CalibrationSettings &settings, const UserToneCurvePreset &preset);

const UserToneCurvePreset *findUserPreset(const std::vector<UserToneCurvePreset> &presets, const std::string &id);

} // namespace AutoHdrCoreCore
