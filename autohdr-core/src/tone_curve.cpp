#include "autohdr/tone_curve.h"
#include "autohdr/tone_curve_presets.h"

#include <algorithm>
#include <cmath>

namespace AutoHdrCore {

namespace {

constexpr float kEpsilon = 1e-6f;
constexpr float kSameXTolerance = 1.0f;

float clampf(float value, float minValue, float maxValue)
{
    return std::max(minValue, std::min(value, maxValue));
}

float stepWidthForReference(float referenceNits)
{
    return std::max(1.0f, referenceNits * 0.005f);
}

struct SegmentSlopes {
    std::vector<float> m;
};

SegmentSlopes computeMonotonicSlopes(const PointList &points)
{
    const int n = static_cast<int>(points.size());
    SegmentSlopes result;
    result.m.resize(n);

    if (n < 2) {
        if (n == 1) {
            result.m[0] = 0.0f;
        }
        return result;
    }

    std::vector<float> h(n - 1);
    std::vector<float> delta(n - 1);
    for (int i = 0; i < n - 1; ++i) {
        h[i] = std::max(points[i + 1].x - points[i].x, kEpsilon);
        delta[i] = (points[i + 1].y - points[i].y) / h[i];
    }

    result.m[0] = delta[0];
    result.m[n - 1] = delta[n - 2];

    for (int i = 1; i < n - 1; ++i) {
        if (delta[i - 1] * delta[i] <= 0.0f) {
            result.m[i] = 0.0f;
        } else {
            const float w1 = 2.0f * h[i] + h[i - 1];
            const float w2 = h[i] + 2.0f * h[i - 1];
            result.m[i] = (w1 + w2) / (w1 / delta[i - 1] + w2 / delta[i]);
        }
    }

    for (int i = 0; i < n - 1; ++i) {
        if (std::abs(delta[i]) < kEpsilon) {
            result.m[i] = 0.0f;
            result.m[i + 1] = 0.0f;
        } else {
            const float alpha = result.m[i] / delta[i];
            const float beta = result.m[i + 1] / delta[i];
            const float tau = alpha * alpha + beta * beta;
            if (tau > 9.0f) {
                const float scale = 3.0f / std::sqrt(tau);
                result.m[i] = scale * alpha * delta[i];
                result.m[i + 1] = scale * beta * delta[i];
            }
        }
    }

    return result;
}

float evaluateHermiteSegment(float x, float x0, float x1, float y0, float y1, float m0, float m1)
{
    const float h = std::max(x1 - x0, kEpsilon);
    const float t = clampf((x - x0) / h, 0.0f, 1.0f);
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;
    return h00 * y0 + h10 * h * m0 + h01 * y1 + h11 * h * m1;
}

int findSegmentIndex(const PointList &points, float inputNits)
{
    if (points.empty()) {
        return -1;
    }
    if (inputNits <= points.front().x) {
        return 0;
    }
    if (inputNits >= points.back().x) {
        return static_cast<int>(points.size()) - 2;
    }
    for (int i = 0; i < static_cast<int>(points.size()) - 1; ++i) {
        if (inputNits >= points[i].x && inputNits <= points[i + 1].x) {
            return i;
        }
    }
    return static_cast<int>(points.size()) - 2;
}

PointList expandStepKnotsForEval(const PointList &points, const ToneCurveEndpoints &endpoints)
{
    PointList sorted = points;
    std::sort(sorted.begin(), sorted.end(), [](const Vec2 &a, const Vec2 &b) { return a.x < b.x; });

    const float maxX = std::max(endpoints.sdrMaxPoint.x - kEpsilon, kEpsilon);
    const float stepWidth = stepWidthForReference(std::max(endpoints.visualReferenceNits, 1.0f));

    PointList expanded;
    expanded.reserve(sorted.size() + 4);

    float groupX = 0.0f;
    float groupYMin = 0.0f;
    float groupYMax = 0.0f;
    bool inGroup = false;
    int groupCount = 0;

    auto flushGroup = [&]() {
        if (!inGroup) {
            return;
        }
        if (groupCount > 1 && groupYMax > groupYMin + kEpsilon) {
            const float stepX = clampf(groupX, kEpsilon, maxX);
            const float preX = std::max(kEpsilon, std::min(stepX - stepWidth, stepX - kEpsilon));
            expanded.push_back({preX, groupYMin});
            expanded.push_back({stepX, groupYMax});
        } else {
            expanded.push_back({groupX, groupYMax});
        }
        inGroup = false;
        groupCount = 0;
    };

    for (const Vec2 &point : sorted) {
        const float x = clampf(point.x, kEpsilon, maxX);
        const float y = clampf(point.y, 0.0f, endpoints.peakNits);

        if (!inGroup) {
            groupX = x;
            groupYMin = y;
            groupYMax = y;
            inGroup = true;
            groupCount = 1;
            continue;
        }

        if (std::abs(x - groupX) <= kSameXTolerance) {
            groupYMin = std::min(groupYMin, y);
            groupYMax = std::max(groupYMax, y);
            groupX = std::max(groupX, x);
            ++groupCount;
            continue;
        }

        flushGroup();
        groupX = x;
        groupYMin = y;
        groupYMax = y;
        inGroup = true;
        groupCount = 1;
    }

    flushGroup();
    return expanded;
}

} // namespace

float clampReferenceNits(float value)
{
    return clampf(value, kReferenceNitsMin, kReferenceNitsMax);
}

float clampBlackPoint(float value)
{
    return clampf(value, -0.01f, 0.01f);
}

float clampVibrance(float value)
{
    return clampf(value, 0.0f, 10.0f);
}

float clampGamutExpansion(float value)
{
    return clampf(value, 0.0f, 20.0f);
}

PointList buildFullCurve(const ToneCurveEndpoints &endpoints, const PointList &intermediate)
{
    const PointList expanded = expandStepKnotsForEval(intermediate, endpoints);
    const PointList mids = sanitizeIntermediatePoints(expanded, endpoints);
    const Vec2 sdrMax = sanitizeSdrMaxPoint(endpoints.sdrMaxPoint, endpoints, mids);

    PointList full;
    full.reserve(mids.size() + 2);
    full.push_back({0.0f, 0.0f});
    for (const Vec2 &point : mids) {
        full.push_back(point);
    }
    full.push_back(sdrMax);
    return full;
}

PointList sanitizeIntermediatePoints(const PointList &points, const ToneCurveEndpoints &endpoints)
{
    PointList sorted = points;
    std::sort(sorted.begin(), sorted.end(), [](const Vec2 &a, const Vec2 &b) { return a.x < b.x; });

    const float maxX = std::max(endpoints.sdrMaxPoint.x - kEpsilon, kEpsilon);
    PointList result;
    result.reserve(sorted.size());

    float lastX = 0.0f;
    float lastY = 0.0f;

    for (const Vec2 &point : sorted) {
        float x = clampf(point.x, kEpsilon, maxX);
        float y = clampf(point.y, 0.0f, endpoints.peakNits);

        if (x <= lastX + kEpsilon) {
            x = lastX + kEpsilon;
        }
        if (y < lastY) {
            y = lastY;
        }
        if (x >= maxX) {
            continue;
        }

        result.push_back({x, y});
        lastX = x;
        lastY = y;
    }

    return result;
}

Vec2 sanitizeSdrMaxPoint(const Vec2 &point, const ToneCurveEndpoints &endpoints, const PointList &intermediate)
{
    float minX = kEpsilon;
    float minY = 0.0f;
    if (!intermediate.empty()) {
        minX = std::max(minX, intermediate.back().x + kEpsilon);
        minY = std::max(minY, intermediate.back().y);
    }

    const float maxInputX = std::max(endpoints.visualReferenceNits, kEpsilon);
    float x = clampf(point.x, minX, maxInputX);
    float y = clampf(point.y, minY, endpoints.peakNits);
    if (y < minY) {
        y = minY;
    }
    return {x, y};
}

float evaluateToneCurve(const PointList &fullCurve, float inputNits)
{
    if (fullCurve.empty()) {
        return inputNits;
    }
    if (fullCurve.size() == 1) {
        return fullCurve.front().y;
    }

    if (inputNits <= fullCurve.front().x) {
        return fullCurve.front().y;
    }
    if (inputNits >= fullCurve.back().x) {
        return fullCurve.back().y;
    }

    if (fullCurve.size() == 2) {
        const float x0 = fullCurve[0].x;
        const float y0 = fullCurve[0].y;
        const float x1 = fullCurve[1].x;
        const float y1 = fullCurve[1].y;
        const float t = clampf((inputNits - x0) / std::max(x1 - x0, kEpsilon), 0.0f, 1.0f);
        return y0 + t * (y1 - y0);
    }

    const SegmentSlopes slopes = computeMonotonicSlopes(fullCurve);
    const int segment = findSegmentIndex(fullCurve, inputNits);
    const int i = std::max(0, std::min(segment, static_cast<int>(fullCurve.size()) - 2));

    return evaluateHermiteSegment(inputNits, fullCurve[i].x, fullCurve[i + 1].x, fullCurve[i].y, fullCurve[i + 1].y,
                                slopes.m[i], slopes.m[i + 1]);
}

void buildToneCurveLut(const PointList &fullCurve, float inputSpan, float *lut, int size)
{
    if (!lut || size <= 0) {
        return;
    }

    const float span = std::max(inputSpan, kEpsilon);
    for (int i = 0; i < size; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(size - 1);
        const float inputNits = u * span;
        lut[i] = evaluateToneCurve(fullCurve, inputNits);
    }
}

namespace {

int roundToInt(float value)
{
    return static_cast<int>(std::lround(value));
}

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

} // namespace

std::string formatToneCurvePoints(const PointList &points)
{
    std::string result;
    for (size_t i = 0; i < points.size(); ++i) {
        if (i > 0) {
            result.push_back(',');
        }
        result += std::to_string(roundToInt(points[i].x));
        result.push_back(':');
        result += std::to_string(roundToInt(points[i].y));
    }
    return result;
}

PointList parseToneCurvePoints(const std::string &encoded)
{
    PointList points;
    for (const std::string &token : splitString(encoded, ',')) {
        const auto colon = token.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        try {
            const float x = std::stof(token.substr(0, colon));
            const float y = std::stof(token.substr(colon + 1));
            points.push_back({x, y});
        } catch (...) {
            continue;
        }
    }
    return points;
}

std::string formatSdrMaxPoint(const Vec2 &point)
{
    return std::to_string(roundToInt(point.x)) + ':' + std::to_string(roundToInt(point.y));
}

Vec2 parseSdrMaxPoint(const std::string &encoded, const Vec2 &fallback)
{
    const auto colon = encoded.find(':');
    if (colon == std::string::npos) {
        return fallback;
    }
    try {
        return {std::stof(encoded.substr(0, colon)), std::stof(encoded.substr(colon + 1))};
    } catch (...) {
        return fallback;
    }
}

void remapToneCurveInputToSdrSpace(CalibrationSettings &settings, float referenceNits, float peakNits)
{
    const float ref = std::max(referenceNits, 1e-3f);
    const float peak = std::max(peakNits, ref);

    const auto remapInputX = [&](float x) {
        if (x <= ref + 1e-3f) {
            return x;
        }
        if (x >= peak * 0.99f) {
            return ref;
        }
        return x * ref / peak;
    };

    settings.sdrMaxPoint.x = remapInputX(settings.sdrMaxPoint.x);
    for (Vec2 &point : settings.toneCurvePoints) {
        point.x = remapInputX(point.x);
    }
}

void sanitizeCalibrationSettings(CalibrationSettings &settings, float maxDisplayNits,
                                 const std::vector<UserToneCurvePreset> *userPresets)
{
    if (settings.referenceNits <= 1e-6f) {
        settings.referenceNits = 203.0f;
    }
    settings.referenceNits = clampReferenceNits(settings.referenceNits);

    const float minPeak = settings.referenceNits + 1.0f;
    settings.maxNits = clampf(settings.maxNits, minPeak, maxDisplayNits);
    settings.vibrance = clampVibrance(settings.vibrance);
    settings.blackPoint = clampBlackPoint(settings.blackPoint);
    settings.gamutExpansion = clampGamutExpansion(settings.gamutExpansion);

    const float peakNits = std::min(settings.maxNits, maxDisplayNits);
    settings.maxNits = peakNits;

    if (settings.toneCurvePreset != ToneCurvePreset::Custom) {
        applyToneCurvePreset(settings, userPresets);
    }

    const float curveReference = settings.referenceNits;
    remapToneCurveInputToSdrSpace(settings, curveReference, peakNits);

    if (settings.sdrMaxPoint.x <= 1e-6f && settings.sdrMaxPoint.y <= 1e-6f) {
        settings.sdrMaxPoint = {curveReference, peakNits};
    }

    ToneCurveEndpoints endpoints;
    endpoints.peakNits = peakNits;
    endpoints.sdrMaxPoint = settings.sdrMaxPoint;
    endpoints.visualReferenceNits = curveReference;
    settings.toneCurvePoints = sanitizeIntermediatePoints(settings.toneCurvePoints, endpoints);
    settings.sdrMaxPoint = sanitizeSdrMaxPoint(settings.sdrMaxPoint, endpoints, settings.toneCurvePoints);
}

ToneCurveEndpoints toneCurveEndpointsFor(const CalibrationSettings &settings, float maxDisplayNits)
{
    ToneCurveEndpoints endpoints;
    endpoints.peakNits = std::min(settings.maxNits, maxDisplayNits);
    endpoints.sdrMaxPoint = settings.sdrMaxPoint;
    endpoints.visualReferenceNits = clampReferenceNits(settings.referenceNits);
    return endpoints;
}

GpuUniforms buildGpuUniforms(const CalibrationSettings &settings, float maxDisplayNits,
                             const std::vector<UserToneCurvePreset> *userPresets)
{
    CalibrationSettings sanitized = settings;
    sanitizeCalibrationSettings(sanitized, maxDisplayNits, userPresets);

    const ToneCurveEndpoints endpoints = toneCurveEndpointsFor(sanitized, maxDisplayNits);
    const PointList fullCurve = buildFullCurve(endpoints, sanitized.toneCurvePoints);

    GpuUniforms uniforms{};
    uniforms.blackPoint = sanitized.blackPoint;
    uniforms.colorVibrance = sanitized.vibrance;
    uniforms.gamutExpansion = sanitized.gamutExpansion;
    uniforms.referenceNits = sanitized.referenceNits;
    uniforms.displayPeak = endpoints.peakNits;

    const float curveSpan = sanitized.sdrMaxPoint.x > 1.0f ? sanitized.sdrMaxPoint.x : sanitized.referenceNits;
    uniforms.toneCurveInputSpan = curveSpan;
    buildToneCurveLut(fullCurve, curveSpan, uniforms.toneCurveLut, kToneCurveLutSize);
    return uniforms;
}

} // namespace AutoHdrCoreCore
