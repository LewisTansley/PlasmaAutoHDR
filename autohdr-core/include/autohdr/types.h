#pragma once

#include <optional>
#include <string>
#include <vector>

namespace AutoHdrCore {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

using PointList = std::vector<Vec2>;

enum class ToneCurvePreset {
    Custom,
    Linear,
    SCurve,
    SCurveBoosted,
    SCurveLifted,
    Exponential,
    User,
};

struct CalibrationSettings {
    float maxNits = 1000.0f;
    float gamutExpansion = 1.5f;
    float blackPoint = 0.0f;
    float vibrance = 0.0f;
    float referenceNits = 203.0f;
    Vec2 sdrMaxPoint{};
    PointList toneCurvePoints;
    ToneCurvePreset toneCurvePreset = ToneCurvePreset::Linear;
    std::string toneCurveUserPresetId;
};

constexpr float kReferenceNitsMin = 80.0f;
constexpr float kReferenceNitsMax = 480.0f;
constexpr int kToneCurveLutSize = 256;

struct AppProfileMetadata {
    std::string key;
    std::string displayName;
    std::string windowClass;
    std::string resourceClass;
    std::string desktopFile;
    std::string steamAppId;
    bool autoActivate = true;
};

struct AppProfile {
    AppProfileMetadata metadata;
    CalibrationSettings settings;
};

struct GeneralSettings {
    bool autoActivateCalibrated = true;
};

struct UserToneCurvePreset {
    std::string id;
    std::string displayName;
    PointList normalizedPoints;
    Vec2 sdrMaxFraction{1.0f, 1.0f};
};

struct DisplayLimits {
    float referenceNits = 203.0f;
    float maxNits = 1000.0f;
};

struct ToneCurveEndpoints {
    float peakNits = 1000.0f;
    Vec2 sdrMaxPoint{};
    float visualReferenceNits = 100.0f;
};

struct GpuUniforms {
    float blackPoint = 0.0f;
    float colorVibrance = 0.0f;
    float gamutExpansion = 0.0f;
    float referenceNits = 203.0f;
    float displayPeak = 1000.0f;
    float toneCurveInputSpan = 203.0f;
    float toneCurveLut[kToneCurveLutSize]{};
};

struct ConfigData {
    GeneralSettings general;
    CalibrationSettings globalSettings;
    std::vector<AppProfile> appProfiles;
    std::vector<UserToneCurvePreset> userPresets;
};

struct RuntimeContext {
    std::string steamAppId;
    std::string executableName;
    std::string profileOverride;
};

} // namespace AutoHdrCoreCore
