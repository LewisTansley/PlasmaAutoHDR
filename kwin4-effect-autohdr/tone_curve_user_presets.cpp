#include "tone_curve_user_presets.h"
#include "tone_curve.h"

#include <KConfigGroup>
#include <QRegularExpression>

namespace AutoHdr {

namespace {

constexpr int kFractionPrecision = 4;

QString formatFraction(float value)
{
    return QString::number(value, 'f', kFractionPrecision);
}

float parseFraction(const QString &text, float fallback)
{
    bool ok = false;
    const float value = text.toFloat(&ok);
    return ok ? value : fallback;
}

} // namespace

QString userPresetComboId(const QString &id)
{
    return QStringLiteral("user:") + id;
}

bool isUserPresetComboId(const QString &comboId)
{
    return comboId.startsWith(QStringLiteral("user:"));
}

QString userPresetIdFromComboId(const QString &comboId)
{
    if (!isUserPresetComboId(comboId)) {
        return QString();
    }
    return comboId.mid(5);
}

QString sanitizeUserPresetKey(const QString &raw)
{
    QString key = raw.trimmed().toLower();
    key.replace(QRegularExpression(QStringLiteral("[^a-z0-9._-]+")), QStringLiteral("_"));
    key.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    key = key.trimmed();
    if (key.isEmpty()) {
        key = QStringLiteral("preset");
    }
    return key;
}

QString userPresetGroupName(const QString &id)
{
    return QString::fromLatin1(presetGroupPrefix) + id;
}

QString formatNormalizedPoints(const QVector<QPointF> &points)
{
    QStringList parts;
    parts.reserve(points.size());
    for (const QPointF &point : points) {
        parts.append(QStringLiteral("%1:%2")
                           .arg(formatFraction(static_cast<float>(point.x())))
                           .arg(formatFraction(static_cast<float>(point.y()))));
    }
    return parts.join(QLatin1Char(','));
}

QVector<QPointF> parseNormalizedPoints(const QString &encoded)
{
    QVector<QPointF> points;
    const QStringList tokens = encoded.split(QLatin1Char(','), Qt::SkipEmptyParts);
    points.reserve(tokens.size());
    for (const QString &token : tokens) {
        const QStringList pair = token.split(QLatin1Char(':'));
        if (pair.size() != 2) {
            continue;
        }
        const float x = parseFraction(pair[0], 0.0f);
        const float y = parseFraction(pair[1], 0.0f);
        points.append(QPointF(x, y));
    }
    return points;
}

QString formatSdrMaxFraction(const QPointF &fraction)
{
    return QStringLiteral("%1:%2")
        .arg(formatFraction(static_cast<float>(fraction.x())))
        .arg(formatFraction(static_cast<float>(fraction.y())));
}

QPointF parseSdrMaxFraction(const QString &encoded, const QPointF &fallback)
{
    const QStringList pair = encoded.split(QLatin1Char(':'));
    if (pair.size() != 2) {
        return fallback;
    }
    return QPointF(parseFraction(pair[0], static_cast<float>(fallback.x())),
                   parseFraction(pair[1], static_cast<float>(fallback.y())));
}

QVector<UserToneCurvePreset> loadUserToneCurvePresets(const KSharedConfigPtr &config)
{
    QVector<UserToneCurvePreset> presets;
    if (!config) {
        return presets;
    }

    const KConfigGroup listGroup(config, QString::fromLatin1(groupUserPresets));
    const QStringList ids = listGroup.readEntry("PresetList", QStringList());
    presets.reserve(ids.size());

    for (const QString &id : ids) {
        const std::optional<UserToneCurvePreset> preset = loadUserToneCurvePreset(config, id);
        if (preset) {
            presets.append(*preset);
        }
    }
    return presets;
}

std::optional<UserToneCurvePreset> loadUserToneCurvePreset(const KSharedConfigPtr &config, const QString &id)
{
    if (!config || id.isEmpty()) {
        return std::nullopt;
    }

    const QString groupName = userPresetGroupName(id);
    if (!config->hasGroup(groupName)) {
        return std::nullopt;
    }

    UserToneCurvePreset preset;
    preset.id = id;
    const KConfigGroup group(config, groupName);
    preset.displayName = group.readEntry("DisplayName", id);
    preset.normalizedPoints = parseNormalizedPoints(group.readEntry("NormalizedPoints", QString()));
    preset.sdrMaxFraction = parseSdrMaxFraction(group.readEntry("SdrMaxFraction", QString()));
    return preset;
}

UserToneCurvePreset normalizeCurrentCurve(const CalibrationSettings &settings)
{
    UserToneCurvePreset preset;
    preset.id = sanitizeUserPresetKey(settings.toneCurveUserPresetId);
    preset.displayName = preset.id;

    const float ref = qMax(settings.referenceNits, 1e-3f);
    const float peak = qMax(settings.maxNits, ref);

    preset.normalizedPoints.reserve(settings.toneCurvePoints.size());
    for (const QPointF &point : settings.toneCurvePoints) {
        preset.normalizedPoints.append(
            QPointF(static_cast<float>(point.x()) / ref, static_cast<float>(point.y()) / peak));
    }

    preset.sdrMaxFraction =
        QPointF(static_cast<float>(settings.sdrMaxPoint.x()) / ref, static_cast<float>(settings.sdrMaxPoint.y()) / peak);
    return preset;
}

void applyUserToneCurvePreset(CalibrationSettings &settings, const UserToneCurvePreset &preset)
{
    const float ref = qMax(settings.referenceNits, 1e-3f);
    const float peak = qMax(settings.maxNits, ref);

    settings.sdrMaxPoint =
        QPointF(static_cast<float>(preset.sdrMaxFraction.x()) * ref, static_cast<float>(preset.sdrMaxFraction.y()) * peak);

    QVector<QPointF> points;
    points.reserve(preset.normalizedPoints.size());
    for (const QPointF &point : preset.normalizedPoints) {
        points.append(QPointF(static_cast<float>(point.x()) * ref, static_cast<float>(point.y()) * peak));
    }

    ToneCurveEndpoints endpoints;
    endpoints.peakNits = peak;
    endpoints.visualReferenceNits = ref;
    endpoints.sdrMaxPoint = settings.sdrMaxPoint;
    settings.toneCurvePoints = sanitizeIntermediatePoints(points, endpoints);
    settings.sdrMaxPoint = sanitizeSdrMaxPoint(settings.sdrMaxPoint, endpoints, settings.toneCurvePoints);
}

bool saveUserToneCurvePreset(const KSharedConfigPtr &config, const UserToneCurvePreset &preset)
{
    if (!config || preset.id.isEmpty()) {
        return false;
    }

    KConfigGroup listGroup(config, QString::fromLatin1(groupUserPresets));
    QStringList ids = listGroup.readEntry("PresetList", QStringList());
    if (!ids.contains(preset.id)) {
        ids.append(preset.id);
        listGroup.writeEntry("PresetList", ids);
    }

    KConfigGroup group(config, userPresetGroupName(preset.id));
    group.writeEntry("DisplayName", preset.displayName.isEmpty() ? preset.id : preset.displayName);
    group.writeEntry("NormalizedPoints", formatNormalizedPoints(preset.normalizedPoints));
    group.writeEntry("SdrMaxFraction", formatSdrMaxFraction(preset.sdrMaxFraction));
    config->sync();
    return true;
}

bool deleteUserToneCurvePreset(const KSharedConfigPtr &config, const QString &id)
{
    if (!config || id.isEmpty()) {
        return false;
    }

    KConfigGroup listGroup(config, QString::fromLatin1(groupUserPresets));
    QStringList ids = listGroup.readEntry("PresetList", QStringList());
    ids.removeAll(id);
    listGroup.writeEntry("PresetList", ids);

    config->deleteGroup(userPresetGroupName(id));
    config->sync();
    return true;
}

} // namespace AutoHdr
