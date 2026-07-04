#pragma once

#include "tone_curve.h"
#include "tone_curve_presets.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QVector4D>
#include <optional>

namespace AutoHdr {

enum class AiQuality {
    Performance = 0,
    Balanced = 1,
    Quality = 2,
};

enum class AiBackend {
    Auto = 0,
    Onnx = 1,
    GlslOnly = 2,
};

struct CalibrationSettings {
    float maxNits = 1000.0f;
    float gamutExpansion = 1.5f;
    float blackPoint = 0.0f;
    float colorIntensity = 0.33f;
    float referenceNits = 203.0f;
    QPointF sdrMaxPoint;
    QVector<QPointF> toneCurvePoints;
    ToneCurvePreset toneCurvePreset = ToneCurvePreset::Linear;
    QString toneCurveUserPresetId;
    bool aiEnhanced = false;
    float aiStrength = 0.5f;
    bool aiChromaEnabled = false;
    float aiChromaStrength = 0.5f;
};

constexpr float kReferenceNitsMin = 80.0f;
constexpr float kReferenceNitsMax = 480.0f;

float clampReferenceNits(float value);
float clampBlackPoint(float value);
float clampGamutExpansion(float value);
float clampColorIntensity(float value);
float clampCurveAntialiasStrength(float value);
float clampHighlightSoftness(float value);
int clampAntiAliasingQuality(int value);
float clampAiStrength(float value);
float clampAiChromaStrength(float value);
AiQuality clampAiQuality(int value);
AiBackend clampAiBackend(int value);
int aiGuidanceScale(AiQuality quality);
int aiInferenceInterval(AiQuality quality);
QString aiQualityToString(AiQuality quality);
AiQuality aiQualityFromString(const QString &value);
QString aiBackendToString(AiBackend backend);
AiBackend aiBackendFromString(const QString &value);

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
    bool perceptualColorEnabled = true;
    float curveAntialiasStrength = 0.45f;
    float highlightSoftness = 0.30f;
    int antiAliasingQuality = 0;
    bool aiEnhanced = false;
    float aiStrength = 0.5f;
    AiQuality aiQuality = AiQuality::Balanced;
    AiBackend aiBackend = AiBackend::Auto;
    bool aiChromaEnabled = false;
    float aiChromaStrength = 0.5f;
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

QVector4D computePqBoostParams(const CalibrationSettings &settings, float referenceNits, float maxDisplayNits);

float linearToPq(float linearNits, float maxPqValue);
float pqToLinear(float pqValue, float maxPqValue);
float computePqMul(float yIn, float yOut, const QVector4D &pqBoostParams);

} // namespace AutoHdr
