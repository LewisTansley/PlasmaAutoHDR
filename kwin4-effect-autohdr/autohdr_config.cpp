#include "autohdr_config.h"
#include "tone_curve_presets.h"
#include "tone_curve_user_presets.h"
#include "tone_curve.h"

#include <KConfigGroup>
#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>

namespace AutoHdr {

float clampReferenceNits(float value)
{
    return qBound(kReferenceNitsMin, value, kReferenceNitsMax);
}

float clampBlackPoint(float value)
{
    return qBound(-0.01f, value, 0.01f);
}

float clampGamutExpansion(float value)
{
    return qBound(0.0f, value, 20.0f);
}

float clampColorIntensity(float value)
{
    return qBound(0.0f, value, 1.0f);
}

float clampCurveAntialiasStrength(float value)
{
    return qBound(0.0f, value, 1.0f);
}

float clampHighlightSoftness(float value)
{
    return qBound(0.0f, value, 1.0f);
}

int clampAntiAliasingQuality(int value)
{
    return qBound(0, value, 2);
}

float clampAiStrength(float value)
{
    return qBound(0.0f, value, 1.0f);
}

float clampAiBandingStrength(float value)
{
    return qBound(0.0f, value, 1.0f);
}

AiQuality clampAiQuality(int value)
{
    return static_cast<AiQuality>(qBound(0, value, 2));
}

AiBackend clampAiBackend(int value)
{
    return static_cast<AiBackend>(qBound(0, value, 2));
}

int aiGuidanceScale(AiQuality quality)
{
    switch (quality) {
    case AiQuality::Performance:
        return 4;
    case AiQuality::Quality:
        return 1;
    case AiQuality::Balanced:
    default:
        return 2;
    }
}

int aiInferenceInterval(AiQuality quality)
{
    switch (quality) {
    case AiQuality::Performance:
        // Periodic refresh on static content; damaged windows update every frame.
        return 2;
    case AiQuality::Quality:
    case AiQuality::Balanced:
    default:
        return 1;
    }
}

QSize aiOrtInferenceSize(AiQuality quality)
{
    switch (quality) {
    case AiQuality::Performance:
        return QSize(128, 128);
    case AiQuality::Quality:
        return QSize(256, 256);
    case AiQuality::Balanced:
    default:
        return QSize(192, 192);
    }
}

int aiOrtSubmitInterval(AiQuality quality)
{
    switch (quality) {
    case AiQuality::Performance:
        return 2;
    case AiQuality::Quality:
    case AiQuality::Balanced:
    default:
        return 1;
    }
}

QString aiQualityToString(AiQuality quality)
{
    switch (quality) {
    case AiQuality::Performance:
        return QStringLiteral("Performance");
    case AiQuality::Quality:
        return QStringLiteral("Quality");
    case AiQuality::Balanced:
    default:
        return QStringLiteral("Balanced");
    }
}

AiQuality aiQualityFromString(const QString &value)
{
    if (value.compare(QStringLiteral("Performance"), Qt::CaseInsensitive) == 0) {
        return AiQuality::Performance;
    }
    if (value.compare(QStringLiteral("Quality"), Qt::CaseInsensitive) == 0) {
        return AiQuality::Quality;
    }
    return AiQuality::Balanced;
}

QString aiBackendToString(AiBackend backend)
{
    switch (backend) {
    case AiBackend::Onnx:
        return QStringLiteral("Onnx");
    case AiBackend::GlslOnly:
        return QStringLiteral("GlslOnly");
    case AiBackend::Auto:
    default:
        return QStringLiteral("Auto");
    }
}

AiBackend aiBackendFromString(const QString &value)
{
    if (value.compare(QStringLiteral("Onnx"), Qt::CaseInsensitive) == 0) {
        return AiBackend::Onnx;
    }
    if (value.compare(QStringLiteral("GlslOnly"), Qt::CaseInsensitive) == 0) {
        return AiBackend::GlslOnly;
    }
    return AiBackend::Auto;
}

AiGuidanceModel clampAiGuidanceModel(int value)
{
    return static_cast<AiGuidanceModel>(qBound(0, value, 3));
}

QString aiGuidanceModelToString(AiGuidanceModel model)
{
    switch (model) {
    case AiGuidanceModel::GuidanceV2:
        return QStringLiteral("GuidanceV2");
    case AiGuidanceModel::GuidanceV1:
        return QStringLiteral("GuidanceV1");
    case AiGuidanceModel::GuidanceV0:
        return QStringLiteral("GuidanceV0");
    case AiGuidanceModel::Latest:
    default:
        return QStringLiteral("Latest");
    }
}

AiGuidanceModel aiGuidanceModelFromString(const QString &value)
{
    if (value.compare(QStringLiteral("GuidanceV2"), Qt::CaseInsensitive) == 0) {
        return AiGuidanceModel::GuidanceV2;
    }
    if (value.compare(QStringLiteral("GuidanceV1"), Qt::CaseInsensitive) == 0) {
        return AiGuidanceModel::GuidanceV1;
    }
    if (value.compare(QStringLiteral("GuidanceV0"), Qt::CaseInsensitive) == 0) {
        return AiGuidanceModel::GuidanceV0;
    }
    return AiGuidanceModel::Latest;
}

QString locateEffectDataFile(const QString &relativePath)
{
    const QStringList candidates =
        QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, relativePath);
    for (const QString &path : candidates) {
        if (!path.startsWith(QDir::homePath())) {
            return path;
        }
    }
    return candidates.isEmpty() ? QString() : candidates.constFirst();
}

namespace {

constexpr const char *kGuidanceModelsDir = "kwin/effects/autohdr/models/";

QString guidanceModelRelativePath(const QString &fileName)
{
    return QString::fromLatin1(kGuidanceModelsDir) + fileName;
}

const QVector<GuidanceModelDescriptor> &registeredGuidanceModels()
{
    static const QVector<GuidanceModelDescriptor> models = {
        {AiGuidanceModel::GuidanceV2,
         2,
         QStringLiteral("guidance_v2.onnx"),
         QStringLiteral("Guidance v2 — trained neural"),
         false,
         true},
        {AiGuidanceModel::GuidanceV1,
         1,
         QStringLiteral("guidance_v1.onnx"),
         QStringLiteral("Guidance v1 — formula (GPU)"),
         true,
         false},
        {AiGuidanceModel::GuidanceV0,
         0,
         QStringLiteral("guidance_v0.onnx"),
         QStringLiteral("Guidance v0 — legacy"),
         true,
         false},
    };
    return models;
}

const GuidanceModelDescriptor *descriptorForModel(AiGuidanceModel model)
{
    if (model == AiGuidanceModel::Latest) {
        return nullptr;
    }
    for (const GuidanceModelDescriptor &descriptor : registeredGuidanceModels()) {
        if (descriptor.id == model) {
            return &descriptor;
        }
    }
    return nullptr;
}

QString resolveLatestGuidanceModelPath()
{
    for (const GuidanceModelDescriptor &descriptor : registeredGuidanceModels()) {
        const QString path = locateEffectDataFile(guidanceModelRelativePath(descriptor.fileName));
        if (!path.isEmpty()) {
            return path;
        }
    }
    return {};
}

} // namespace

QVector<GuidanceModelDescriptor> guidanceModelDescriptors()
{
    return registeredGuidanceModels();
}

QString resolveGuidanceModelPath(AiGuidanceModel model)
{
    if (model == AiGuidanceModel::Latest) {
        return resolveLatestGuidanceModelPath();
    }

    const GuidanceModelDescriptor *descriptor = descriptorForModel(model);
    if (!descriptor) {
        return resolveLatestGuidanceModelPath();
    }
    return locateEffectDataFile(guidanceModelRelativePath(descriptor->fileName));
}

std::optional<GuidanceModelDescriptor> guidanceModelFromPath(const QString &modelPath)
{
    if (modelPath.isEmpty()) {
        return std::nullopt;
    }
    for (const GuidanceModelDescriptor &descriptor : registeredGuidanceModels()) {
        if (modelPath.contains(descriptor.fileName)) {
            return descriptor;
        }
    }
    return std::nullopt;
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
    general.perceptualColorEnabled = group.readEntry("PerceptualColorEnabled", true);
    general.curveAntialiasStrength =
        clampCurveAntialiasStrength(group.readEntry("CurveAntialiasStrength", 0.45f));
    general.highlightSoftness = clampHighlightSoftness(group.readEntry("HighlightSoftness", 0.30f));
    general.antiAliasingQuality = clampAntiAliasingQuality(group.readEntry("AntiAliasingQuality", 0));
    general.aiEnhanced = group.readEntry("AiEnhanced", false);
    general.aiStrength = clampAiStrength(group.readEntry("AiStrength", 0.5f));
    general.aiQuality = aiQualityFromString(group.readEntry("AiQuality", QStringLiteral("Balanced")));
    general.aiBackend = aiBackendFromString(group.readEntry("AiBackend", QStringLiteral("Auto")));
    general.aiGuidanceModel =
        aiGuidanceModelFromString(group.readEntry("AiGuidanceModel", QStringLiteral("Latest")));
    general.aiBandingStrength = clampAiBandingStrength(group.readEntry("AiBandingStrength", 0.7f));
    return general;
}

void saveGeneralSettings(const KSharedConfigPtr &config, const GeneralSettings &general)
{
    KConfigGroup group(config, QString::fromLatin1(groupGeneral));
    group.writeEntry("AutoActivateCalibrated", general.autoActivateCalibrated);
    group.writeEntry("PerceptualColorEnabled", general.perceptualColorEnabled);
    group.writeEntry("CurveAntialiasStrength", general.curveAntialiasStrength);
    group.writeEntry("HighlightSoftness", general.highlightSoftness);
    group.writeEntry("AntiAliasingQuality", general.antiAliasingQuality);
    group.writeEntry("AiEnhanced", general.aiEnhanced);
    group.writeEntry("AiStrength", general.aiStrength);
    group.writeEntry("AiQuality", aiQualityToString(general.aiQuality));
    group.writeEntry("AiBackend", aiBackendToString(general.aiBackend));
    group.writeEntry("AiGuidanceModel", aiGuidanceModelToString(general.aiGuidanceModel));
    group.writeEntry("AiBandingStrength", general.aiBandingStrength);
    config->sync();
}

void readCalibrationFromGroup(const KConfigGroup &group, CalibrationSettings &settings, float defaultMaxNits,
                              const KSharedConfigPtr &config)
{
    settings.maxNits = group.readEntry("MaxNits", defaultMaxNits);
    settings.gamutExpansion = group.readEntry("GamutExpansion", 1.5f);
    settings.blackPoint = group.readEntry("BlackPoint", 0.0f);
    settings.colorIntensity = group.readEntry("ColorIntensity", 0.33f);
    settings.aiEnhanced = group.readEntry("AiEnhanced", false);
    settings.aiStrength = clampAiStrength(group.readEntry("AiStrength", 0.5f));

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
    group.writeEntry("ColorIntensity", settings.colorIntensity);
    group.writeEntry("AiEnhanced", settings.aiEnhanced);
    group.writeEntry("AiStrength", settings.aiStrength);
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
    settings.colorIntensity = clampColorIntensity(settings.colorIntensity);
    settings.blackPoint = clampBlackPoint(settings.blackPoint);
    settings.gamutExpansion = clampGamutExpansion(settings.gamutExpansion);
    settings.aiStrength = clampAiStrength(settings.aiStrength);

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

namespace {

constexpr float kPqN = 2610.0f / 4096.0f / 4.0f;
constexpr float kPqRcpN = 1.0f / kPqN;
constexpr float kPqM = 2523.0f / 4096.0f * 128.0f;
constexpr float kPqRcpM = 1.0f / kPqM;
constexpr float kPqC1 = 3424.0f / 4096.0f;
constexpr float kPqC2 = 2413.0f / 4096.0f * 32.0f;
constexpr float kPqC3 = 2392.0f / 4096.0f * 32.0f;

constexpr float kPqBoost0 = 1.0f;
constexpr float kPqBoost1 = 0.1f;
constexpr float kPqBoost3 = 0.5f;

} // namespace

float linearToPq(float linearNits, float maxPqValue)
{
    const float normalized = std::pow(qMax(linearNits, 0.0f) / qMax(maxPqValue, 1e-6f), kPqN);
    const float nd = (kPqC1 + kPqC2 * normalized) / (1.0f + kPqC3 * normalized);
    return std::pow(nd, kPqM);
}

float pqToLinear(float pqValue, float maxPqValue)
{
    const float pq = std::pow(qMax(pqValue, 0.0f), kPqRcpM);
    const float nd = qMax(pq - kPqC1, 0.0f) / (kPqC2 - kPqC3 * pq);
    return std::pow(nd, kPqRcpN) * maxPqValue;
}

float computePqMul(float yIn, float yOut, const QVector4D &pqBoostParams)
{
    const float p0 = pqBoostParams.x();
    const float p1 = pqBoostParams.y();
    const float p3 = pqBoostParams.z();

    const float pqIn = linearToPq(yIn, p0);
    const float pqOut = linearToPq(yOut * p3, p1);
    return pqOut / qMax(pqIn, 1e-6f);
}

QVector4D computePqBoostParams(const CalibrationSettings &settings, float referenceNits, float maxDisplayNits)
{
    Q_UNUSED(settings)
    Q_UNUSED(referenceNits)
    Q_UNUSED(maxDisplayNits)

    return QVector4D(kPqBoost0, kPqBoost1, kPqBoost3, 0.0f);
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
