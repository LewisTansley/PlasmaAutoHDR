#pragma once

#include <KConfigGroup>
#include <KSharedConfig>
#include <QString>
#include <QStringList>
#include <optional>

namespace AutoHdr {

struct CalibrationSettings {
    float maxNits = 1000.0f;
    float gamutExpansion = 1.5f;
    float blackPoint = 0.0f;
    float vibrance = 0.0f;
    float midPoint = 203.0f;
    float highlightExpansion = 1.0f;
    float highlightLift = 1.0f;
    float highlightRange = 0.0f;
};

constexpr float kHighlightExpansionMin = 0.5f;
constexpr float kHighlightExpansionMax = 2.0f;
constexpr float kHighlightLiftMin = 0.5f;
constexpr float kHighlightLiftMax = 20.0f;
constexpr float kHighlightRangeMax = 3.0f;

struct AppProfileMetadata {
    QString key;
    QString displayName;
    QString windowClass;
    QString resourceClass;
    QString desktopFile;
    bool autoActivate = true;
};

struct AppProfile {
    AppProfileMetadata metadata;
    CalibrationSettings settings;
};

struct GeneralSettings {
    bool autoActivateCalibrated = true;
};

constexpr const char *configFileName = "kwin4effectautohdr";
constexpr const char *groupGeneral = "General";
constexpr const char *groupSettings = "Settings";
constexpr const char *groupApplications = "Applications";
constexpr const char *appGroupPrefix = "App ";

KSharedConfigPtr openConfig();

QString sanitizeAppKey(const QString &raw);
QString appGroupName(const QString &key);

GeneralSettings loadGeneralSettings(const KSharedConfigPtr &config);
void saveGeneralSettings(const KSharedConfigPtr &config, const GeneralSettings &general);

CalibrationSettings loadGlobalSettings(const KSharedConfigPtr &config, float defaultMaxNits);
void saveGlobalSettings(const KSharedConfigPtr &config, const CalibrationSettings &settings);

QStringList listCalibratedApps(const KSharedConfigPtr &config);
bool hasAppProfile(const KSharedConfigPtr &config, const QString &key);
std::optional<AppProfile> loadAppProfile(const KSharedConfigPtr &config, const QString &key);
void saveAppProfile(const KSharedConfigPtr &config, const AppProfile &profile);
void deleteAppProfile(const KSharedConfigPtr &config, const QString &key);

QString findAppKeyForIdentifiers(const KSharedConfigPtr &config, const QString &desktopFile,
                                 const QString &resourceClass, const QString &windowClass);

void readCalibrationFromGroup(const KConfigGroup &group, CalibrationSettings &settings, float defaultMaxNits);
void writeCalibrationToGroup(KConfigGroup &group, const CalibrationSettings &settings);

void sanitizeCalibrationSettings(CalibrationSettings &settings, float referenceNits, float maxDisplayNits);

float migrateMidPoint(float value);
float effectiveMaxNits(const CalibrationSettings &settings, float referenceNits, float maxDisplayNits);

} // namespace AutoHdr
