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

constexpr float kExponentialRate = 2.4f;

struct BuiltInCalibratedCurve {
    ToneCurvePreset preset;
    QVector<QPointF> normalizedPoints;
};

const BuiltInCalibratedCurve kCalibratedCurves[] = {
    {ToneCurvePreset::Balanced,
     {QPointF(0.2405f, 0.0492f), QPointF(0.7548f, 0.7492f)}},
    {ToneCurvePreset::LiftedShadows,
     {QPointF(0.2491f, 0.1302f), QPointF(0.7462f, 0.7492f)}},
    {ToneCurvePreset::SoftShadows,
     {QPointF(0.3562f, 0.1397f), QPointF(0.7537f, 0.7490f)}},
    {ToneCurvePreset::VividHighlights,
     {QPointF(0.3605f, 0.1444f), QPointF(0.7033f, 0.7492f)}},
    {ToneCurvePreset::HighContrast,
     {QPointF(0.2533f, 0.0587f), QPointF(0.7044f, 0.7490f)}},
};

const BuiltInCalibratedCurve *calibratedCurveFor(ToneCurvePreset preset)
{
    for (const BuiltInCalibratedCurve &curve : kCalibratedCurves) {
        if (curve.preset == preset) {
            return &curve;
        }
    }
    return nullptr;
}

float exponentialMapping(float t)
{
    const float k = kExponentialRate;
    const float denom = std::exp(k) - 1.0f;
    if (std::abs(denom) < 1e-6f) {
        return t;
    }
    return (std::exp(k * t) - 1.0f) / denom;
}

QVector<QPointF> generateCalibratedIntermediatePoints(const BuiltInCalibratedCurve &curve,
                                                      const PresetCurveParams &params)
{
    const float ref = qMax(params.referenceNits, 1e-3f);
    const float peak = qMax(params.peakNits, ref);

    QVector<QPointF> points;
    points.reserve(curve.normalizedPoints.size());
    for (const QPointF &point : curve.normalizedPoints) {
        points.append(QPointF(static_cast<float>(point.x()) * ref, static_cast<float>(point.y()) * peak));
    }

    ToneCurveEndpoints endpoints;
    endpoints.peakNits = peak;
    endpoints.visualReferenceNits = ref;
    endpoints.sdrMaxPoint = QPointF(ref, peak);
    return sanitizeIntermediatePoints(points, endpoints);
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
    case ToneCurvePreset::Balanced:
        return QStringLiteral("balanced");
    case ToneCurvePreset::LiftedShadows:
        return QStringLiteral("lifted_shadows");
    case ToneCurvePreset::SoftShadows:
        return QStringLiteral("soft_shadows");
    case ToneCurvePreset::VividHighlights:
        return QStringLiteral("vivid_highlights");
    case ToneCurvePreset::HighContrast:
        return QStringLiteral("high_contrast");
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
    if (normalized == QLatin1String("balanced") || normalized == QLatin1String("scurve")) {
        return ToneCurvePreset::Balanced;
    }
    if (normalized == QLatin1String("lifted_shadows") || normalized == QLatin1String("scurve_boosted")) {
        return ToneCurvePreset::LiftedShadows;
    }
    if (normalized == QLatin1String("soft_shadows") || normalized == QLatin1String("scurve_lifted")) {
        return ToneCurvePreset::SoftShadows;
    }
    if (normalized == QLatin1String("vivid_highlights")) {
        return ToneCurvePreset::VividHighlights;
    }
    if (normalized == QLatin1String("high_contrast")) {
        return ToneCurvePreset::HighContrast;
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
    return {ToneCurvePreset::Linear, ToneCurvePreset::Balanced, ToneCurvePreset::LiftedShadows,
            ToneCurvePreset::SoftShadows, ToneCurvePreset::VividHighlights, ToneCurvePreset::HighContrast,
            ToneCurvePreset::Exponential};
}

QString presetDisplayName(ToneCurvePreset preset)
{
    switch (preset) {
    case ToneCurvePreset::Linear:
        return QStringLiteral("Linear");
    case ToneCurvePreset::Balanced:
        return QStringLiteral("Balanced");
    case ToneCurvePreset::LiftedShadows:
        return QStringLiteral("Lifted Shadows");
    case ToneCurvePreset::SoftShadows:
        return QStringLiteral("Soft Shadows");
    case ToneCurvePreset::VividHighlights:
        return QStringLiteral("Vivid Highlights");
    case ToneCurvePreset::HighContrast:
        return QStringLiteral("High Contrast");
    case ToneCurvePreset::Exponential:
        return QStringLiteral("Exponential");
    case ToneCurvePreset::User:
        return QStringLiteral("User Preset");
    case ToneCurvePreset::Custom:
        return QStringLiteral("Custom");
    }
    return QStringLiteral("Custom");
}

std::optional<ToneCurvePreset> builtInPresetForLegacyUserId(const QString &userPresetId)
{
    const QString normalized = userPresetId.trimmed().toLower();
    if (normalized == QLatin1String("s_curve_dark")) {
        return ToneCurvePreset::Balanced;
    }
    if (normalized == QLatin1String("s_boosted_low")) {
        return ToneCurvePreset::LiftedShadows;
    }
    if (normalized == QLatin1String("s_flat_low")) {
        return ToneCurvePreset::SoftShadows;
    }
    if (normalized == QLatin1String("s_flat_low_boosted_high")) {
        return ToneCurvePreset::VividHighlights;
    }
    if (normalized == QLatin1String("s_dark_boosted_high")) {
        return ToneCurvePreset::HighContrast;
    }
    return std::nullopt;
}

QVector<QPointF> generatePresetIntermediatePoints(ToneCurvePreset preset, const PresetCurveParams &params)
{
    if (preset == ToneCurvePreset::Custom || preset == ToneCurvePreset::Linear || preset == ToneCurvePreset::User) {
        return {};
    }

    if (const BuiltInCalibratedCurve *curve = calibratedCurveFor(preset)) {
        return generateCalibratedIntermediatePoints(*curve, params);
    }

    if (preset != ToneCurvePreset::Exponential) {
        return {};
    }

    const float ref = qMax(params.referenceNits, 1e-3f);
    const float peak = qMax(params.peakNits, ref);

    QVector<QPointF> points;
    points.reserve(kSampleCount);
    for (float t : kSampleFractions) {
        const float x = t * ref;
        const float y = exponentialMapping(t) * peak;
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
