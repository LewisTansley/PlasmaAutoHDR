#pragma once

#include "tone_curve.h"
#include "tone_curve_presets.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

namespace AutoHdr {

struct CalibrationSettings {
    float maxNits = 1000.0f;
    float gamutExpansion = 1.5f;
    float blackPoint = 0.0f;
    float vibrance = 0.0f;
    float referenceNits = 203.0f;
    float chromaCompensation = 0.0f;
    float highlightRolloff = 0.0f;
    float gamutMappingStrength = 0.0f;
    QPointF sdrMaxPoint;
    QVector<QPointF> toneCurvePoints;
    ToneCurvePreset toneCurvePreset = ToneCurvePreset::Linear;
    QString toneCurveUserPresetId;
};

constexpr float kReferenceNitsMin = 80.0f;
constexpr float kReferenceNitsMax = 480.0f;

float clampReferenceNits(float value);
float clampBlackPoint(float value);
float clampVibrance(float value);
float clampGamutExpansion(float value);
float clampChromaCompensation(float value);
float clampHighlightRolloff(float value);
float clampGamutMappingStrength(float value);
float clampPostCurveDebandStrength(float value);

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
    int processingQuality = 0;
    float debandStrength = 0.25f;
    float ditherStrength = 0.15f / 255.0f;
    float postCurveDebandStrength = 0.0f;
};

constexpr const char *configFileName = "kwin4effectautohdr";
constexpr const char *groupGeneral = "General";
constexpr const char *groupSettings = "Settings";
constexpr const char *groupApplications = "Applications";
constexpr const char *groupUserPresets = "UserPresets";
constexpr const char *appGroupPrefix = "App ";
constexpr const char *presetGroupPrefix = "Preset ";

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

void readCalibrationFromGroup(const KConfigGroup &group, CalibrationSettings &settings, float defaultMaxNits,
                              const KSharedConfigPtr &config = {});
void writeCalibrationToGroup(KConfigGroup &group, const CalibrationSettings &settings);

void sanitizeCalibrationSettings(CalibrationSettings &settings, float referenceNits, float maxDisplayNits,
                                 const KSharedConfigPtr &config = {});

ToneCurveEndpoints toneCurveEndpointsFor(const CalibrationSettings &settings, float hdrReferenceNits,
                                         float maxDisplayNits);

} // namespace AutoHdr
