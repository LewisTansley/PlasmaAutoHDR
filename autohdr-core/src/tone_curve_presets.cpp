#include "autohdr/tone_curve_presets.h"

#include "autohdr/tone_curve.h"
#include "autohdr/tone_curve_user_presets.h"

#include <algorithm>
#include <cmath>

namespace AutoHdrCore {

namespace {

constexpr float kSampleFractions[] = {0.25f, 0.5f, 0.75f};
constexpr int kSampleCount = 3;

constexpr float kSCurveBoostStrength = 0.65f;
constexpr float kSCurveLiftAmount = 0.12f;
constexpr float kExponentialRate = 2.4f;

float smoothstep(float t)
{
    t = std::max(0.0f, std::min(t, 1.0f));
    return t * t * (3.0f - 2.0f * t);
}

float presetMapping(ToneCurvePreset preset, float t)
{
    t = std::max(0.0f, std::min(t, 1.0f));

    switch (preset) {
    case ToneCurvePreset::Linear:
        return t;
    case ToneCurvePreset::SCurve:
        return smoothstep(t);
    case ToneCurvePreset::SCurveBoosted: {
        const float s = smoothstep(t);
        return t + kSCurveBoostStrength * (s - t);
    }
    case ToneCurvePreset::SCurveLifted:
        return kSCurveLiftAmount + (1.0f - kSCurveLiftAmount) * smoothstep(t);
    case ToneCurvePreset::Exponential: {
        const float k = kExponentialRate;
        const float denom = std::exp(k) - 1.0f;
        if (std::abs(denom) < 1e-6f) {
            return t;
        }
        return (std::exp(k * t) - 1.0f) / denom;
    }
    case ToneCurvePreset::Custom:
    case ToneCurvePreset::User:
        break;
    }
    return t;
}

bool pointsMatch(const PointList &a, const PointList &b, float toleranceNits)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i].x - b[i].x) > toleranceNits) {
            return false;
        }
        if (std::abs(a[i].y - b[i].y) > toleranceNits) {
            return false;
        }
    }
    return true;
}

bool sdrMaxMatchesPreset(const CalibrationSettings &settings, float toleranceNits)
{
    const float ref = settings.referenceNits;
    const float peak = settings.maxNits;
    return std::abs(settings.sdrMaxPoint.x - ref) <= toleranceNits
        && std::abs(settings.sdrMaxPoint.y - peak) <= toleranceNits;
}

} // namespace

std::string presetToString(ToneCurvePreset preset)
{
    switch (preset) {
    case ToneCurvePreset::Linear:
        return "linear";
    case ToneCurvePreset::SCurve:
        return "scurve";
    case ToneCurvePreset::SCurveBoosted:
        return "scurve_boosted";
    case ToneCurvePreset::SCurveLifted:
        return "scurve_lifted";
    case ToneCurvePreset::Exponential:
        return "exponential";
    case ToneCurvePreset::User:
        return "user";
    case ToneCurvePreset::Custom:
        return "custom";
    }
    return "custom";
}

ToneCurvePreset presetFromString(const std::string &encoded)
{
    std::string normalized = encoded;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (normalized == "linear") {
        return ToneCurvePreset::Linear;
    }
    if (normalized == "scurve") {
        return ToneCurvePreset::SCurve;
    }
    if (normalized == "scurve_boosted") {
        return ToneCurvePreset::SCurveBoosted;
    }
    if (normalized == "scurve_lifted") {
        return ToneCurvePreset::SCurveLifted;
    }
    if (normalized == "exponential") {
        return ToneCurvePreset::Exponential;
    }
    if (normalized == "user") {
        return ToneCurvePreset::User;
    }
    if (normalized == "custom") {
        return ToneCurvePreset::Custom;
    }
    return ToneCurvePreset::Custom;
}

std::vector<ToneCurvePreset> builtInToneCurvePresets()
{
    return {ToneCurvePreset::Linear, ToneCurvePreset::SCurve, ToneCurvePreset::SCurveBoosted,
            ToneCurvePreset::SCurveLifted, ToneCurvePreset::Exponential};
}

std::string presetDisplayName(ToneCurvePreset preset)
{
    switch (preset) {
    case ToneCurvePreset::Linear:
        return "Linear";
    case ToneCurvePreset::SCurve:
        return "S-Curve";
    case ToneCurvePreset::SCurveBoosted:
        return "S-Curve Boosted";
    case ToneCurvePreset::SCurveLifted:
        return "S-Curve Lifted";
    case ToneCurvePreset::Exponential:
        return "Exponential";
    case ToneCurvePreset::User:
        return "User Preset";
    case ToneCurvePreset::Custom:
        return "Custom";
    }
    return "Custom";
}

PointList generatePresetIntermediatePoints(ToneCurvePreset preset, const PresetCurveParams &params)
{
    if (preset == ToneCurvePreset::Custom || preset == ToneCurvePreset::Linear || preset == ToneCurvePreset::User) {
        return {};
    }

    const float ref = std::max(params.referenceNits, 1e-3f);
    const float peak = std::max(params.peakNits, ref);

    PointList points;
    points.reserve(kSampleCount);
    for (float t : kSampleFractions) {
        const float x = t * ref;
        const float y = presetMapping(preset, t) * peak;
        points.push_back({x, y});
    }

    ToneCurveEndpoints endpoints;
    endpoints.peakNits = peak;
    endpoints.visualReferenceNits = ref;
    endpoints.sdrMaxPoint = {ref, peak};
    return sanitizeIntermediatePoints(points, endpoints);
}

void applyToneCurvePreset(CalibrationSettings &settings, const std::vector<UserToneCurvePreset> *userPresets)
{
    if (settings.toneCurvePreset == ToneCurvePreset::Custom) {
        return;
    }

    if (settings.toneCurvePreset == ToneCurvePreset::User) {
        if (!userPresets || settings.toneCurveUserPresetId.empty()) {
            return;
        }
        const UserToneCurvePreset *preset = findUserPreset(*userPresets, settings.toneCurveUserPresetId);
        if (!preset) {
            return;
        }
        applyUserToneCurvePreset(settings, *preset);
        return;
    }

    const float ref = settings.referenceNits;
    const float peak = settings.maxNits;
    settings.sdrMaxPoint = {ref, peak};

    PresetCurveParams params;
    params.referenceNits = ref;
    params.peakNits = peak;
    settings.toneCurvePoints = generatePresetIntermediatePoints(settings.toneCurvePreset, params);
}

ToneCurvePreset detectMatchingPreset(const CalibrationSettings &settings, float toleranceNits,
                                     const std::vector<UserToneCurvePreset> *userPresets)
{
    if (!sdrMaxMatchesPreset(settings, toleranceNits)) {
        return ToneCurvePreset::Custom;
    }

    if (settings.toneCurvePoints.empty()) {
        return ToneCurvePreset::Linear;
    }

    PresetCurveParams params;
    params.referenceNits = settings.referenceNits;
    params.peakNits = settings.maxNits;

    for (ToneCurvePreset preset : builtInToneCurvePresets()) {
        if (preset == ToneCurvePreset::Linear) {
            continue;
        }
        const PointList generated = generatePresetIntermediatePoints(preset, params);
        if (pointsMatch(settings.toneCurvePoints, generated, toleranceNits)) {
            return preset;
        }
    }

    if (userPresets) {
        for (const UserToneCurvePreset &userPreset : *userPresets) {
            CalibrationSettings probe = settings;
            applyUserToneCurvePreset(probe, userPreset);
            if (pointsMatch(settings.toneCurvePoints, probe.toneCurvePoints, toleranceNits)
                && std::abs(settings.sdrMaxPoint.x - probe.sdrMaxPoint.x) <= toleranceNits
                && std::abs(settings.sdrMaxPoint.y - probe.sdrMaxPoint.y) <= toleranceNits) {
                return ToneCurvePreset::User;
            }
        }
    }

    return ToneCurvePreset::Custom;
}

} // namespace AutoHdrCoreCore
