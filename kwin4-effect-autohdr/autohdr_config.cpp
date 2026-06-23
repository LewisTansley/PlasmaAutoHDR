#include "autohdr_config.h"

#include <KConfigGroup>
#include <QRegularExpression>

namespace AutoHdr {

namespace {

float clampBlackPoint(float value)
{
    return qBound(-0.01f, value, 0.01f);
}

float clampVibrance(float value)
{
    return qBound(0.0f, value, 10.0f);
}

float clampHighlightExpansion(float value)
{
    return qBound(kHighlightExpansionMin, value, kHighlightExpansionMax);
}

float clampHighlightLift(float value)
{
    return qBound(kHighlightLiftMin, value, kHighlightLiftMax);
}

float clampHighlightRange(float value)
{
    return qBound(0.0f, value, kHighlightRangeMax);
}

float clampGamutExpansion(float value)
{
    return qBound(0.0f, value, 20.0f);
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

void readCalibrationFromGroup(const KConfigGroup &group, CalibrationSettings &settings, float defaultMaxNits)
{
    settings.maxNits = group.readEntry("MaxNits", defaultMaxNits);
    settings.gamutExpansion = group.readEntry("GamutExpansion", 1.5f);
    settings.blackPoint = group.readEntry("BlackPoint", 0.0f);
    settings.vibrance = group.readEntry("Vibrance", 0.0f);
    settings.midPoint = migrateMidPoint(static_cast<float>(group.readEntry("MidPoint", 203)));
    settings.highlightExpansion = clampHighlightExpansion(group.readEntry("HighlightExpansion", 1.0f));
    settings.highlightLift = clampHighlightLift(group.readEntry("HighlightLift", 1.0f));
    settings.highlightRange = clampHighlightRange(group.readEntry("HighlightRange", 0.0f));
}

void writeCalibrationToGroup(KConfigGroup &group, const CalibrationSettings &settings)
{
    group.writeEntry("MaxNits", settings.maxNits);
    group.writeEntry("GamutExpansion", settings.gamutExpansion);
    group.writeEntry("BlackPoint", settings.blackPoint);
    group.writeEntry("Vibrance", settings.vibrance);
    group.writeEntry("MidPoint", qRound(settings.midPoint));
    group.writeEntry("HighlightExpansion", settings.highlightExpansion);
    group.writeEntry("HighlightLift", settings.highlightLift);
    group.writeEntry("HighlightRange", settings.highlightRange);
}

CalibrationSettings loadGlobalSettings(const KSharedConfigPtr &config, float defaultMaxNits)
{
    CalibrationSettings settings;
    readCalibrationFromGroup(KConfigGroup(config, QString::fromLatin1(groupSettings)), settings, defaultMaxNits);
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
    readCalibrationFromGroup(group, profile.settings, profile.settings.maxNits);
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

void sanitizeCalibrationSettings(CalibrationSettings &settings, float referenceNits, float maxDisplayNits)
{
    if (settings.maxNits <= referenceNits * 1.01f) {
        settings.maxNits = maxDisplayNits;
    } else if (settings.maxNits > maxDisplayNits) {
        settings.maxNits = maxDisplayNits;
    }
    settings.midPoint = migrateMidPoint(settings.midPoint);
    settings.vibrance = clampVibrance(settings.vibrance);
    settings.blackPoint = clampBlackPoint(settings.blackPoint);
    settings.highlightExpansion = clampHighlightExpansion(settings.highlightExpansion);
    settings.highlightLift = clampHighlightLift(settings.highlightLift);
    settings.highlightRange = clampHighlightRange(settings.highlightRange);
    if (settings.gamutExpansion < 0.0f || settings.gamutExpansion > 20.0f) {
        settings.gamutExpansion = 1.5f;
    }
}

float effectiveMaxNits(const CalibrationSettings &settings, float referenceNits, float maxDisplayNits)
{
    if (settings.maxNits <= referenceNits * 1.01f) {
        return maxDisplayNits;
    }
    return qMin(settings.maxNits, maxDisplayNits);
}

} // namespace AutoHdr
