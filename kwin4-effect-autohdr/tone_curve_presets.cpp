#include "tone_curve_presets.h"
#include "autohdr_config.h"
#include "tone_curve.h"
#include "tone_curve_user_presets.h"

#include <QtGlobal>

#include <cmath>

namespace AutoHdr {

namespace {

constexpr float kSampleFractions[] = {0.25f, 0.5f, 0.75f};
constexpr int kSampleCount = 3;

constexpr float kSCurveBoostStrength = 0.65f;
constexpr float kSCurveLiftAmount = 0.12f;
constexpr float kExponentialRate = 2.4f;

float smoothstep(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

float presetMapping(ToneCurvePreset preset, float t)
{
    t = qBound(0.0f, t, 1.0f);

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

bool pointsMatch(const QVector<QPointF> &a, const QVector<QPointF> &b, float toleranceNits)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (int i = 0; i < a.size(); ++i) {
        if (qAbs(static_cast<float>(a[i].x()) - static_cast<float>(b[i].x())) > toleranceNits) {
            return false;
        }
        if (qAbs(static_cast<float>(a[i].y()) - static_cast<float>(b[i].y())) > toleranceNits) {
            return false;
        }
    }
    return true;
}

bool sdrMaxMatchesPreset(const CalibrationSettings &settings, float toleranceNits)
{
    const float ref = settings.referenceNits;
    const float peak = settings.maxNits;
    return qAbs(static_cast<float>(settings.sdrMaxPoint.x()) - ref) <= toleranceNits
        && qAbs(static_cast<float>(settings.sdrMaxPoint.y()) - peak) <= toleranceNits;
}

} // namespace

QString presetToString(ToneCurvePreset preset)
{
    switch (preset) {
    case ToneCurvePreset::Linear:
        return QStringLiteral("linear");
    case ToneCurvePreset::SCurve:
        return QStringLiteral("scurve");
    case ToneCurvePreset::SCurveBoosted:
        return QStringLiteral("scurve_boosted");
    case ToneCurvePreset::SCurveLifted:
        return QStringLiteral("scurve_lifted");
    case ToneCurvePreset::Exponential:
        return QStringLiteral("exponential");
    case ToneCurvePreset::User:
        return QStringLiteral("user");
    case ToneCurvePreset::Custom:
        return QStringLiteral("custom");
    }
    return QStringLiteral("custom");
}

ToneCurvePreset presetFromString(const QString &encoded)
{
    const QString normalized = encoded.trimmed().toLower();
    if (normalized == QLatin1String("linear")) {
        return ToneCurvePreset::Linear;
    }
    if (normalized == QLatin1String("scurve")) {
        return ToneCurvePreset::SCurve;
    }
    if (normalized == QLatin1String("scurve_boosted")) {
        return ToneCurvePreset::SCurveBoosted;
    }
    if (normalized == QLatin1String("scurve_lifted")) {
        return ToneCurvePreset::SCurveLifted;
    }
    if (normalized == QLatin1String("exponential")) {
        return ToneCurvePreset::Exponential;
    }
    if (normalized == QLatin1String("user")) {
        return ToneCurvePreset::User;
    }
    if (normalized == QLatin1String("custom")) {
        return ToneCurvePreset::Custom;
    }
    return ToneCurvePreset::Custom;
}

QVector<ToneCurvePreset> builtInToneCurvePresets()
{
    return {ToneCurvePreset::Linear, ToneCurvePreset::SCurve, ToneCurvePreset::SCurveBoosted,
            ToneCurvePreset::SCurveLifted, ToneCurvePreset::Exponential};
}

QString presetDisplayName(ToneCurvePreset preset)
{
    switch (preset) {
    case ToneCurvePreset::Linear:
        return QStringLiteral("Linear");
    case ToneCurvePreset::SCurve:
        return QStringLiteral("S-Curve");
    case ToneCurvePreset::SCurveBoosted:
        return QStringLiteral("S-Curve Boosted");
    case ToneCurvePreset::SCurveLifted:
        return QStringLiteral("S-Curve Lifted");
    case ToneCurvePreset::Exponential:
        return QStringLiteral("Exponential");
    case ToneCurvePreset::User:
        return QStringLiteral("User Preset");
    case ToneCurvePreset::Custom:
        return QStringLiteral("Custom");
    }
    return QStringLiteral("Custom");
}

QVector<QPointF> generatePresetIntermediatePoints(ToneCurvePreset preset, const PresetCurveParams &params)
{
    if (preset == ToneCurvePreset::Custom || preset == ToneCurvePreset::Linear || preset == ToneCurvePreset::User) {
        return {};
    }

    const float ref = qMax(params.referenceNits, 1e-3f);
    const float peak = qMax(params.peakNits, ref);

    QVector<QPointF> points;
    points.reserve(kSampleCount);
    for (float t : kSampleFractions) {
        const float x = t * ref;
        const float y = presetMapping(preset, t) * peak;
        points.append(QPointF(x, y));
    }

    ToneCurveEndpoints endpoints;
    endpoints.peakNits = peak;
    endpoints.visualReferenceNits = ref;
    endpoints.sdrMaxPoint = QPointF(ref, peak);
    return sanitizeIntermediatePoints(points, endpoints);
}

void applyToneCurvePreset(CalibrationSettings &settings, const KSharedConfigPtr &config)
{
    if (settings.toneCurvePreset == ToneCurvePreset::Custom) {
        return;
    }

    if (settings.toneCurvePreset == ToneCurvePreset::User) {
        if (!config || settings.toneCurveUserPresetId.isEmpty()) {
            return;
        }
        const std::optional<UserToneCurvePreset> preset =
            loadUserToneCurvePreset(config, settings.toneCurveUserPresetId);
        if (!preset) {
            return;
        }
        applyUserToneCurvePreset(settings, *preset);
        return;
    }

    const float ref = settings.referenceNits;
    const float peak = settings.maxNits;
    settings.sdrMaxPoint = QPointF(ref, peak);

    PresetCurveParams params;
    params.referenceNits = ref;
    params.peakNits = peak;
    settings.toneCurvePoints = generatePresetIntermediatePoints(settings.toneCurvePreset, params);
}

ToneCurvePreset detectMatchingPreset(const CalibrationSettings &settings, float toleranceNits,
                                     const KSharedConfigPtr &config)
{
    if (!sdrMaxMatchesPreset(settings, toleranceNits)) {
        return ToneCurvePreset::Custom;
    }

    if (settings.toneCurvePoints.isEmpty()) {
        return ToneCurvePreset::Linear;
    }

    PresetCurveParams params;
    params.referenceNits = settings.referenceNits;
    params.peakNits = settings.maxNits;

    for (ToneCurvePreset preset : builtInToneCurvePresets()) {
        if (preset == ToneCurvePreset::Linear) {
            continue;
        }
        const QVector<QPointF> generated = generatePresetIntermediatePoints(preset, params);
        if (pointsMatch(settings.toneCurvePoints, generated, toleranceNits)) {
            return preset;
        }
    }

    if (config) {
        for (const UserToneCurvePreset &userPreset : loadUserToneCurvePresets(config)) {
            CalibrationSettings probe = settings;
            applyUserToneCurvePreset(probe, userPreset);
            if (pointsMatch(settings.toneCurvePoints, probe.toneCurvePoints, toleranceNits)
                && qAbs(static_cast<float>(settings.sdrMaxPoint.x()) - static_cast<float>(probe.sdrMaxPoint.x()))
                    <= toleranceNits
                && qAbs(static_cast<float>(settings.sdrMaxPoint.y()) - static_cast<float>(probe.sdrMaxPoint.y()))
                    <= toleranceNits) {
                return ToneCurvePreset::User;
            }
        }
    }

    return ToneCurvePreset::Custom;
}

} // namespace AutoHdr
