#include "autohdr/tone_curve_user_presets.h"

#include "autohdr/config.h"
#include "autohdr/tone_curve.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <regex>
#include <sstream>

namespace AutoHdrCore {

namespace {

constexpr int kFractionPrecision = 4;

std::vector<std::string> splitString(const std::string &text, char delimiter)
{
    std::vector<std::string> parts;
    std::string current;
    for (char ch : text) {
        if (ch == delimiter) {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

std::string formatFraction(float value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(kFractionPrecision) << value;
    return stream.str();
}

float parseFraction(const std::string &text, float fallback)
{
    try {
        return std::stof(text);
    } catch (...) {
        return fallback;
    }
}

} // namespace

std::string userPresetComboId(const std::string &id)
{
    return "user:" + id;
}

bool isUserPresetComboId(const std::string &comboId)
{
    return comboId.rfind("user:", 0) == 0;
}

std::string userPresetIdFromComboId(const std::string &comboId)
{
    if (!isUserPresetComboId(comboId)) {
        return {};
    }
    return comboId.substr(5);
}

std::string sanitizeUserPresetKey(const std::string &raw)
{
    std::string key = raw;
    key.erase(key.begin(), std::find_if(key.begin(), key.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    key.erase(std::find_if(key.rbegin(), key.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), key.end());
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    key = std::regex_replace(key, std::regex(R"([^a-z0-9._-]+)"), "_");
    key = std::regex_replace(key, std::regex(R"(_+)"), "_");
    if (key.empty()) {
        key = "preset";
    }
    return key;
}

std::string formatNormalizedPoints(const PointList &points)
{
    std::string result;
    for (size_t i = 0; i < points.size(); ++i) {
        if (i > 0) {
            result.push_back(',');
        }
        result += formatFraction(points[i].x);
        result.push_back(':');
        result += formatFraction(points[i].y);
    }
    return result;
}

PointList parseNormalizedPoints(const std::string &encoded)
{
    PointList points;
    for (const std::string &token : splitString(encoded, ',')) {
        const auto colon = token.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        points.push_back({parseFraction(token.substr(0, colon), 0.0f),
                          parseFraction(token.substr(colon + 1), 0.0f)});
    }
    return points;
}

std::string formatSdrMaxFraction(const Vec2 &fraction)
{
    return formatFraction(fraction.x) + ':' + formatFraction(fraction.y);
}

Vec2 parseSdrMaxFraction(const std::string &encoded, const Vec2 &fallback)
{
    const auto colon = encoded.find(':');
    if (colon == std::string::npos) {
        return fallback;
    }
    return {parseFraction(encoded.substr(0, colon), fallback.x),
            parseFraction(encoded.substr(colon + 1), fallback.y)};
}

UserToneCurvePreset normalizeCurrentCurve(const CalibrationSettings &settings)
{
    UserToneCurvePreset preset;
    preset.id = sanitizeUserPresetKey(settings.toneCurveUserPresetId);
    preset.displayName = preset.id;

    const float ref = std::max(settings.referenceNits, 1e-3f);
    const float peak = std::max(settings.maxNits, ref);

    preset.normalizedPoints.reserve(settings.toneCurvePoints.size());
    for (const Vec2 &point : settings.toneCurvePoints) {
        preset.normalizedPoints.push_back({point.x / ref, point.y / peak});
    }

    preset.sdrMaxFraction = {settings.sdrMaxPoint.x / ref, settings.sdrMaxPoint.y / peak};
    return preset;
}

void applyUserToneCurvePreset(CalibrationSettings &settings, const UserToneCurvePreset &preset)
{
    const float ref = std::max(settings.referenceNits, 1e-3f);
    const float peak = std::max(settings.maxNits, ref);

    settings.sdrMaxPoint = {preset.sdrMaxFraction.x * ref, preset.sdrMaxFraction.y * peak};

    PointList points;
    points.reserve(preset.normalizedPoints.size());
    for (const Vec2 &point : preset.normalizedPoints) {
        points.push_back({point.x * ref, point.y * peak});
    }

    ToneCurveEndpoints endpoints;
    endpoints.peakNits = peak;
    endpoints.visualReferenceNits = ref;
    endpoints.sdrMaxPoint = settings.sdrMaxPoint;
    settings.toneCurvePoints = sanitizeIntermediatePoints(points, endpoints);
    settings.sdrMaxPoint = sanitizeSdrMaxPoint(settings.sdrMaxPoint, endpoints, settings.toneCurvePoints);
}

const UserToneCurvePreset *findUserPreset(const std::vector<UserToneCurvePreset> &presets, const std::string &id)
{
    for (const UserToneCurvePreset &preset : presets) {
        if (preset.id == id) {
            return &preset;
        }
    }
    return nullptr;
}

} // namespace AutoHdrCoreCore
