#pragma once

#include "types.h"

#include <string>

namespace AutoHdrCore {

float clampReferenceNits(float value);
float clampBlackPoint(float value);
float clampVibrance(float value);
float clampGamutExpansion(float value);

PointList buildFullCurve(const ToneCurveEndpoints &endpoints, const PointList &intermediate);
PointList sanitizeIntermediatePoints(const PointList &points, const ToneCurveEndpoints &endpoints);
Vec2 sanitizeSdrMaxPoint(const Vec2 &point, const ToneCurveEndpoints &endpoints, const PointList &intermediate);

float evaluateToneCurve(const PointList &fullCurve, float inputNits);
void buildToneCurveLut(const PointList &fullCurve, float inputSpan, float *lut, int size);

std::string formatToneCurvePoints(const PointList &points);
PointList parseToneCurvePoints(const std::string &encoded);
std::string formatSdrMaxPoint(const Vec2 &point);
Vec2 parseSdrMaxPoint(const std::string &encoded, const Vec2 &fallback);

void remapToneCurveInputToSdrSpace(CalibrationSettings &settings, float referenceNits, float peakNits);
void sanitizeCalibrationSettings(CalibrationSettings &settings, float maxDisplayNits,
                                 const std::vector<UserToneCurvePreset> *userPresets = nullptr);
ToneCurveEndpoints toneCurveEndpointsFor(const CalibrationSettings &settings, float maxDisplayNits);
GpuUniforms buildGpuUniforms(const CalibrationSettings &settings, float maxDisplayNits,
                             const std::vector<UserToneCurvePreset> *userPresets = nullptr);

} // namespace AutoHdrCoreCore
