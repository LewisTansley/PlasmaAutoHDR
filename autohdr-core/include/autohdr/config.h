#pragma once

#include "types.h"

#include <optional>
#include <string>
#include <unordered_map>

namespace AutoHdrCore {

constexpr const char *configFileName = "kwin4effectautohdr";
constexpr const char *groupGeneral = "General";
constexpr const char *groupSettings = "Settings";
constexpr const char *groupApplications = "Applications";
constexpr const char *groupUserPresets = "UserPresets";
constexpr const char *appGroupPrefix = "App ";
constexpr const char *presetGroupPrefix = "Preset ";

std::string defaultConfigPath();
std::string sanitizeAppKey(const std::string &raw);
std::string appGroupName(const std::string &key);
std::string userPresetGroupName(const std::string &id);
std::string steamAppProfileKey(const std::string &steamAppId);

bool loadConfigFromFile(const std::string &path, ConfigData &out);
bool saveGlobalSettingsToFile(const std::string &path, const CalibrationSettings &settings);
bool saveAppProfileToFile(const std::string &path, const AppProfile &profile);

CalibrationSettings readCalibrationFromEntries(const std::unordered_map<std::string, std::string> &entries,
                                               float defaultMaxNits, const ConfigData *fullConfig = nullptr);
void writeCalibrationToEntries(std::unordered_map<std::string, std::string> &entries,
                               const CalibrationSettings &settings);

std::optional<AppProfile> findAppProfile(const ConfigData &config, const std::string &key);
CalibrationSettings resolveSettings(const ConfigData &config, const RuntimeContext &ctx, float maxDisplayNits);
std::string findAppKeyForIdentifiers(const ConfigData &config, const std::string &desktopFile,
                                     const std::string &resourceClass, const std::string &windowClass,
                                     const std::string &steamAppId = {});

} // namespace AutoHdrCoreCore
