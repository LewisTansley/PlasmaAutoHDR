#include "autohdr_config.h"
#include "tone_curve_presets.h"
#include "tone_curve_user_presets.h"
#include "tone_curve.h"

#include <KConfigGroup>
#include <QRegularExpression>

namespace AutoHdr {

float clampReferenceNits(float value)
{
    return qBound(kReferenceNitsMin, value, kReferenceNitsMax);
}

float clampBlackPoint(float value)
{
    return qBound(-0.01f, value, 0.01f);
}

float clampVibrance(float value)
{
    return qBound(0.0f, value, 10.0f);
}

float clampGamutExpansion(float value)
{
    return qBound(0.0f, value, 20.0f);
}

float clampChromaCompensation(float value)
{
    return qBound(0.0f, value, 1.0f);
}

float clampHighlightRolloff(float value)
{
    return qBound(0.0f, value, 1.0f);
}

float clampGamutMappingStrength(float value)
{
    return qBound(0.0f, value, 1.0f);
}

namespace {

QPointF migrateSdrMaxPoint(const KConfigGroup &group, float peakNits)
{
    const QString encoded = group.readEntry("SdrMaxPoint", QString());
    if (!encoded.isEmpty()) {
        return parseSdrMaxPoint(encoded, QPointF(peakNits, peakNits));
    }

    const float legacyOutput = group.readEntry("MaxEndpointOutput", peakNits);
    return QPointF(peakNits, legacyOutput);
}

float migrateMidPoint(float value)
{
    if (value <= 1.0f) {
        if (value >= 0.25f && value <= 0.75f) {
            return 80.f + (value - 0.25f) / 0.5f * 400.f;
        }
        return 203.f;
    }
    return qBound(80.f, value, 480.f);
}

void seedToneCurveFromLegacy(CalibrationSettings &settings, float legacyMidPoint)
{
    const float peak = settings.maxNits;
    settings.referenceNits = clampReferenceNits(legacyMidPoint);
    settings.sdrMaxPoint = QPointF(settings.referenceNits, peak);
    settings.toneCurvePoints.clear();
    settings.toneCurvePreset = ToneCurvePreset::Linear;
}

void remapToneCurveInputToSdrSpace(CalibrationSettings &settings, float referenceNits, float peakNits)
{
    const float ref = qMax(referenceNits, 1e-3f);
    const float peak = qMax(peakNits, ref);

    const auto remapInputX = [&](float x) {
        if (x <= ref + 1e-3f) {
            return x;
        }
        if (x >= peak * 0.99f) {
            return ref;
        }
        return x * ref / peak;
    };

    settings.sdrMaxPoint.setX(remapInputX(static_cast<float>(settings.sdrMaxPoint.x())));
    for (QPointF &point : settings.toneCurvePoints) {
        point.setX(remapInputX(static_cast<float>(point.x())));
    }
}

bool identifiersMatch(const AppProfileMetadata &metadata, const QString &desktopFile, const QString &resourceClass,
                      const QString &windowClass)
{
    if (!desktopFile.isEmpty() && !metadata.desktopFile.isEmpty()) {
        const QString normalizedDesktop = desktopFile.endsWith(QStringLiteral(".desktop"))
            ? desktopFile.chopped(8)
            : desktopFile;
        const QString storedDesktop = metadata.desktopFile.endsWith(QStringLiteral(".desktop"))
            ? metadata.desktopFile.chopped(8)
            : metadata.desktopFile;
        if (normalizedDesktop.compare(storedDesktop, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }

    if (!resourceClass.isEmpty() && !metadata.resourceClass.isEmpty()
        && resourceClass.compare(metadata.resourceClass, Qt::CaseInsensitive) == 0) {
        return true;
    }

    if (!windowClass.isEmpty() && !metadata.windowClass.isEmpty()) {
        const QStringList parts = windowClass.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QString classPart = parts.isEmpty() ? windowClass : parts.constLast();
        if (classPart.compare(metadata.windowClass, Qt::CaseInsensitive) == 0) {
            return true;
        }
        if (windowClass.compare(metadata.windowClass, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }

    return false;
}

} // namespace

KSharedConfigPtr openConfig()
{
    return KSharedConfig::openConfig(QString::fromLatin1(configFileName));
}

QString sanitizeAppKey(const QString &raw)
{
    QString key = raw.trimmed().toLower();
    key.replace(QRegularExpression(QStringLiteral("[^a-z0-9._-]+")), QStringLiteral("_"));
    key.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    key = key.trimmed();
    if (key.isEmpty()) {
        key = QStringLiteral("unknown");
    }
    return key;
}

QString appGroupName(const QString &key)
{
    return QString::fromLatin1(appGroupPrefix) + key;
}

GeneralSettings loadGeneralSettings(const KSharedConfigPtr &config)
{
    GeneralSettings general;
    const KConfigGroup group(config, QString::fromLatin1(groupGeneral));
    general.autoActivateCalibrated = group.readEntry("AutoActivateCalibrated", true);
    return general;
}

void saveGeneralSettings(const KSharedConfigPtr &config, const GeneralSettings &general)
{
    KConfigGroup group(config, QString::fromLatin1(groupGeneral));
    group.writeEntry("AutoActivateCalibrated", general.autoActivateCalibrated);
    config->sync();
}

void readCalibrationFromGroup(const KConfigGroup &group, CalibrationSettings &settings, float defaultMaxNits,
                              const KSharedConfigPtr &config)
{
    settings.maxNits = group.readEntry("MaxNits", defaultMaxNits);
    settings.gamutExpansion = group.readEntry("GamutExpansion", 1.5f);
    settings.blackPoint = group.readEntry("BlackPoint", 0.0f);
    settings.vibrance = group.readEntry("Vibrance", 0.0f);
    settings.chromaCompensation = group.readEntry("ChromaCompensation", 0.0f);
    settings.highlightRolloff = group.readEntry("HighlightRolloff", 0.0f);
    settings.gamutMappingStrength = group.readEntry("GamutMappingStrength", 0.0f);

    const float legacyMidPoint = migrateMidPoint(static_cast<float>(group.readEntry("MidPoint", 203)));
    settings.toneCurvePoints = parseToneCurvePoints(group.readEntry("ToneCurvePoints", QString()));
    settings.sdrMaxPoint = migrateSdrMaxPoint(group, settings.maxNits);

    const bool hasReferenceNits = group.hasKey(QStringLiteral("ReferenceNits"));
    const bool hasSdrMaxPointKey =
        group.hasKey(QStringLiteral("SdrMaxPoint")) || group.hasKey(QStringLiteral("MaxEndpointOutput"));

    settings.referenceNits = hasReferenceNits
        ? static_cast<float>(group.readEntry("ReferenceNits", qRound(legacyMidPoint)))
        : legacyMidPoint;

    if (!hasReferenceNits || !hasSdrMaxPointKey) {
        seedToneCurveFromLegacy(settings, legacyMidPoint);
    } else if (group.hasKey(QStringLiteral("ToneCurvePreset"))) {
        settings.toneCurvePreset = presetFromString(group.readEntry("ToneCurvePreset"));
        settings.toneCurveUserPresetId = group.readEntry("ToneCurveUserPresetId", QString());
        if (settings.toneCurvePreset == ToneCurvePreset::User) {
            if (const std::optional<ToneCurvePreset> migrated =
                    builtInPresetForLegacyUserId(settings.toneCurveUserPresetId)) {
                settings.toneCurvePreset = *migrated;
                settings.toneCurveUserPresetId.clear();
            }
        }
        if (settings.toneCurvePreset != ToneCurvePreset::Custom) {
            applyToneCurvePreset(settings, config);
        }
    } else {
        settings.toneCurvePreset = ToneCurvePreset::Custom;
    }
}

void writeCalibrationToGroup(KConfigGroup &group, const CalibrationSettings &settings)
{
    group.writeEntry("MaxNits", settings.maxNits);
    group.writeEntry("GamutExpansion", settings.gamutExpansion);
    group.writeEntry("BlackPoint", settings.blackPoint);
    group.writeEntry("Vibrance", settings.vibrance);
    group.writeEntry("ChromaCompensation", settings.chromaCompensation);
    group.writeEntry("HighlightRolloff", settings.highlightRolloff);
    group.writeEntry("GamutMappingStrength", settings.gamutMappingStrength);
    group.writeEntry("ReferenceNits", qRound(settings.referenceNits));
    group.writeEntry("SdrMaxPoint", formatSdrMaxPoint(settings.sdrMaxPoint));
    group.writeEntry("ToneCurvePoints", formatToneCurvePoints(settings.toneCurvePoints));
    group.writeEntry("ToneCurvePreset", presetToString(settings.toneCurvePreset));
    if (settings.toneCurvePreset == ToneCurvePreset::User) {
        group.writeEntry("ToneCurveUserPresetId", settings.toneCurveUserPresetId);
    } else {
        group.deleteEntry("ToneCurveUserPresetId");
    }
}

CalibrationSettings loadGlobalSettings(const KSharedConfigPtr &config, float defaultMaxNits)
{
    CalibrationSettings settings;
    readCalibrationFromGroup(KConfigGroup(config, QString::fromLatin1(groupSettings)), settings, defaultMaxNits,
                             config);
    return settings;
}

void saveGlobalSettings(const KSharedConfigPtr &config, const CalibrationSettings &settings)
{
    KConfigGroup group(config, QString::fromLatin1(groupSettings));
    writeCalibrationToGroup(group, settings);
    config->sync();
}

QStringList listCalibratedApps(const KSharedConfigPtr &config)
{
    const KConfigGroup group(config, QString::fromLatin1(groupApplications));
    return group.readEntry("AppList", QStringList());
}

bool hasAppProfile(const KSharedConfigPtr &config, const QString &key)
{
    return config->hasGroup(appGroupName(key));
}

std::optional<AppProfile> loadAppProfile(const KSharedConfigPtr &config, const QString &key)
{
    const QString groupName = appGroupName(key);
    if (!config->hasGroup(groupName)) {
        return std::nullopt;
    }

    AppProfile profile;
    profile.metadata.key = key;
    const KConfigGroup group(config, groupName);
    profile.metadata.displayName = group.readEntry("DisplayName", key);
    profile.metadata.windowClass = group.readEntry("WindowClass", QString());
    profile.metadata.resourceClass = group.readEntry("ResourceClass", QString());
    profile.metadata.desktopFile = group.readEntry("DesktopFile", QString());
    profile.metadata.autoActivate = group.readEntry("AutoActivate", true);
    readCalibrationFromGroup(group, profile.settings, profile.settings.maxNits, config);
    return profile;
}

void saveAppProfile(const KSharedConfigPtr &config, const AppProfile &profile)
{
    KConfigGroup appsGroup(config, QString::fromLatin1(groupApplications));
    QStringList appList = appsGroup.readEntry("AppList", QStringList());
    if (!appList.contains(profile.metadata.key)) {
        appList.append(profile.metadata.key);
        appsGroup.writeEntry("AppList", appList);
    }

    KConfigGroup group(config, appGroupName(profile.metadata.key));
    group.writeEntry("DisplayName", profile.metadata.displayName);
    group.writeEntry("WindowClass", profile.metadata.windowClass);
    group.writeEntry("ResourceClass", profile.metadata.resourceClass);
    group.writeEntry("DesktopFile", profile.metadata.desktopFile);
    group.writeEntry("AutoActivate", profile.metadata.autoActivate);
    writeCalibrationToGroup(group, profile.settings);
    config->sync();
}

void deleteAppProfile(const KSharedConfigPtr &config, const QString &key)
{
    KConfigGroup appsGroup(config, QString::fromLatin1(groupApplications));
    QStringList appList = appsGroup.readEntry("AppList", QStringList());
    appList.removeAll(key);
    appsGroup.writeEntry("AppList", appList);

    config->deleteGroup(appGroupName(key));
    config->sync();
}

QString findAppKeyForIdentifiers(const KSharedConfigPtr &config, const QString &desktopFile,
                                 const QString &resourceClass, const QString &windowClass)
{
    for (const QString &key : listCalibratedApps(config)) {
        const std::optional<AppProfile> profile = loadAppProfile(config, key);
        if (!profile) {
            continue;
        }
        if (identifiersMatch(profile->metadata, desktopFile, resourceClass, windowClass)) {
            return key;
        }
    }
    return QString();
}

void sanitizeCalibrationSettings(CalibrationSettings &settings, float referenceNits, float maxDisplayNits,
                               const KSharedConfigPtr &config)
{
    Q_UNUSED(referenceNits)

    if (settings.referenceNits <= 1e-6f) {
        settings.referenceNits = 203.0f;
    }
    settings.referenceNits = clampReferenceNits(settings.referenceNits);

    const float minPeak = settings.referenceNits + 1.0f;
    settings.maxNits = qBound(minPeak, settings.maxNits, maxDisplayNits);
    settings.vibrance = clampVibrance(settings.vibrance);
    settings.blackPoint = clampBlackPoint(settings.blackPoint);
    settings.gamutExpansion = clampGamutExpansion(settings.gamutExpansion);
    settings.chromaCompensation = clampChromaCompensation(settings.chromaCompensation);
    settings.highlightRolloff = clampHighlightRolloff(settings.highlightRolloff);
    settings.gamutMappingStrength = clampGamutMappingStrength(settings.gamutMappingStrength);

    const float peakNits = qMin(settings.maxNits, maxDisplayNits);
    settings.maxNits = peakNits;

    if (settings.toneCurvePreset != ToneCurvePreset::Custom) {
        applyToneCurvePreset(settings, config);
    }

    const float curveReference = settings.referenceNits;

    remapToneCurveInputToSdrSpace(settings, curveReference, peakNits);

    if (settings.sdrMaxPoint.x() <= 1e-6f && settings.sdrMaxPoint.y() <= 1e-6f) {
        settings.sdrMaxPoint = QPointF(curveReference, peakNits);
    }

    ToneCurveEndpoints endpoints;
    endpoints.peakNits = peakNits;
    endpoints.sdrMaxPoint = settings.sdrMaxPoint;
    endpoints.visualReferenceNits = curveReference;
    settings.toneCurvePoints = sanitizeIntermediatePoints(settings.toneCurvePoints, endpoints);
    settings.sdrMaxPoint = sanitizeSdrMaxPoint(settings.sdrMaxPoint, endpoints, settings.toneCurvePoints);
}

ToneCurveEndpoints toneCurveEndpointsFor(const CalibrationSettings &settings, float hdrReferenceNits,
                                         float maxDisplayNits)
{
    Q_UNUSED(hdrReferenceNits)

    ToneCurveEndpoints endpoints;
    endpoints.peakNits = qMin(settings.maxNits, maxDisplayNits);
    endpoints.sdrMaxPoint = settings.sdrMaxPoint;
    endpoints.visualReferenceNits = clampReferenceNits(settings.referenceNits);
    return endpoints;
}

} // namespace AutoHdr
