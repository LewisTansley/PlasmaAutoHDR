#include "autohdr/config.h"

#include "autohdr/display_limits.h"
#include "autohdr/tone_curve.h"
#include "autohdr/tone_curve_presets.h"
#include "autohdr/tone_curve_user_presets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

namespace AutoHdrCore {

namespace {

using IniStore = std::unordered_map<std::string, std::unordered_map<std::string, std::string>>;

std::string trim(const std::string &value)
{
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

bool iequals(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool loadIniFile(const std::string &path, IniStore &store)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    std::string currentGroup = "General";
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            currentGroup = line.substr(1, line.size() - 2);
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        store[currentGroup][key] = value;
    }
    return true;
}

bool writeIniFile(const std::string &path, const IniStore &store)
{
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    for (const auto &[group, entries] : store) {
        file << '[' << group << "]\n";
        for (const auto &[key, value] : entries) {
            file << key << '=' << value << '\n';
        }
        file << '\n';
    }
    return true;
}

const std::unordered_map<std::string, std::string> &groupEntries(const IniStore &store, const std::string &group)
{
    static const std::unordered_map<std::string, std::string> empty;
    const auto it = store.find(group);
    return it == store.end() ? empty : it->second;
}

std::string readEntry(const IniStore &store, const std::string &group, const std::string &key,
                      const std::string &fallback = {})
{
    const auto &entries = groupEntries(store, group);
    const auto it = entries.find(key);
    return it == entries.end() ? fallback : it->second;
}

bool hasKey(const IniStore &store, const std::string &group, const std::string &key)
{
    const auto &entries = groupEntries(store, group);
    return entries.find(key) != entries.end();
}

float readFloat(const IniStore &store, const std::string &group, const std::string &key, float fallback)
{
    try {
        return std::stof(readEntry(store, group, key, std::to_string(fallback)));
    } catch (...) {
        return fallback;
    }
}

bool readBool(const IniStore &store, const std::string &group, const std::string &key, bool fallback)
{
    const std::string value = readEntry(store, group, key);
    if (value.empty()) {
        return fallback;
    }
    return value == "true" || value == "1";
}

std::vector<std::string> splitCommaList(const std::string &encoded)
{
    std::vector<std::string> parts;
    std::stringstream stream(encoded);
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = trim(item);
        if (!item.empty()) {
            parts.push_back(item);
        }
    }
    return parts;
}

float migrateMidPoint(float value)
{
    if (value <= 1.0f) {
        if (value >= 0.25f && value <= 0.75f) {
            return 80.f + (value - 0.25f) / 0.5f * 400.f;
        }
        return 203.f;
    }
    return std::max(80.f, std::min(480.f, value));
}

Vec2 migrateSdrMaxPoint(const std::unordered_map<std::string, std::string> &entries, float peakNits)
{
    const auto it = entries.find("SdrMaxPoint");
    if (it != entries.end() && !it->second.empty()) {
        return parseSdrMaxPoint(it->second, {peakNits, peakNits});
    }

    const auto legacy = entries.find("MaxEndpointOutput");
    if (legacy != entries.end()) {
        try {
            return {peakNits, std::stof(legacy->second)};
        } catch (...) {
        }
    }
    return {peakNits, peakNits};
}

void seedToneCurveFromLegacy(CalibrationSettings &settings, float legacyMidPoint)
{
    const float peak = settings.maxNits;
    settings.referenceNits = clampReferenceNits(legacyMidPoint);
    settings.sdrMaxPoint = {settings.referenceNits, peak};
    settings.toneCurvePoints.clear();
    settings.toneCurvePreset = ToneCurvePreset::Linear;
}

bool identifiersMatch(const AppProfileMetadata &metadata, const std::string &desktopFile,
                        const std::string &resourceClass, const std::string &windowClass,
                        const std::string &steamAppId)
{
    if (!steamAppId.empty()) {
        if (!metadata.steamAppId.empty() && iequals(metadata.steamAppId, steamAppId)) {
            return true;
        }
        const std::string steamKey = steamAppProfileKey(steamAppId);
        if (iequals(metadata.key, steamKey)) {
            return true;
        }
        if (!metadata.desktopFile.empty()) {
            std::string normalized = metadata.desktopFile;
            if (normalized.size() > 8 && iequals(normalized.substr(normalized.size() - 8), ".desktop")) {
                normalized = normalized.substr(0, normalized.size() - 8);
            }
            if (iequals(normalized, steamKey)) {
                return true;
            }
        }
    }

    if (!desktopFile.empty() && !metadata.desktopFile.empty()) {
        std::string normalizedDesktop = desktopFile;
        if (normalizedDesktop.size() > 8 && iequals(normalizedDesktop.substr(normalizedDesktop.size() - 8), ".desktop")) {
            normalizedDesktop = normalizedDesktop.substr(0, normalizedDesktop.size() - 8);
        }
        std::string storedDesktop = metadata.desktopFile;
        if (storedDesktop.size() > 8 && iequals(storedDesktop.substr(storedDesktop.size() - 8), ".desktop")) {
            storedDesktop = storedDesktop.substr(0, storedDesktop.size() - 8);
        }
        if (iequals(normalizedDesktop, storedDesktop)) {
            return true;
        }
    }

    if (!resourceClass.empty() && !metadata.resourceClass.empty() && iequals(resourceClass, metadata.resourceClass)) {
        return true;
    }

    if (!windowClass.empty() && !metadata.windowClass.empty()) {
        const auto space = windowClass.find_last_of(' ');
        const std::string classPart = space == std::string::npos ? windowClass : windowClass.substr(space + 1);
        if (iequals(classPart, metadata.windowClass) || iequals(windowClass, metadata.windowClass)) {
            return true;
        }
    }

    return false;
}

UserToneCurvePreset loadUserPresetFromStore(const IniStore &store, const std::string &id)
{
    UserToneCurvePreset preset;
    preset.id = id;
    const std::string group = userPresetGroupName(id);
    preset.displayName = readEntry(store, group, "DisplayName", id);
    preset.normalizedPoints = parseNormalizedPoints(readEntry(store, group, "NormalizedPoints"));
    preset.sdrMaxFraction = parseSdrMaxFraction(readEntry(store, group, "SdrMaxFraction"));
    return preset;
}

} // namespace

std::string defaultConfigPath()
{
    if (const char *overridePath = std::getenv("AUTOHDR_VK_CONFIG")) {
        return overridePath;
    }
    if (const char *home = std::getenv("HOME")) {
        return std::string(home) + "/.config/" + configFileName;
    }
    return std::string("/home/deck/.config/") + configFileName;
}

std::string sanitizeAppKey(const std::string &raw)
{
    std::string key = raw;
    key.erase(key.begin(), std::find_if(key.begin(), key.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    key.erase(std::find_if(key.rbegin(), key.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), key.end());
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    key = std::regex_replace(key, std::regex(R"([^a-z0-9._-]+)"), "_");
    key = std::regex_replace(key, std::regex(R"(_+)"), "_");
    if (key.empty()) {
        key = "unknown";
    }
    return key;
}

std::string appGroupName(const std::string &key)
{
    return std::string(appGroupPrefix) + key;
}

std::string userPresetGroupName(const std::string &id)
{
    return std::string(presetGroupPrefix) + id;
}

std::string steamAppProfileKey(const std::string &steamAppId)
{
    return sanitizeAppKey("steam_app_" + steamAppId);
}

CalibrationSettings readCalibrationFromEntries(const std::unordered_map<std::string, std::string> &entries,
                                                 float defaultMaxNits, const ConfigData *fullConfig)
{
    CalibrationSettings settings;
    settings.maxNits = entries.count("MaxNits") ? std::stof(entries.at("MaxNits")) : defaultMaxNits;
    settings.gamutExpansion = entries.count("GamutExpansion") ? std::stof(entries.at("GamutExpansion")) : 1.5f;
    settings.blackPoint = entries.count("BlackPoint") ? std::stof(entries.at("BlackPoint")) : 0.0f;
    settings.vibrance = entries.count("Vibrance") ? std::stof(entries.at("Vibrance")) : 0.0f;

    const bool useToneCurve = entries.count("UseToneCurve") && entries.at("UseToneCurve") == "true";
    const float legacyMidPoint = migrateMidPoint(entries.count("MidPoint") ? std::stof(entries.at("MidPoint")) : 203.0f);
    settings.toneCurvePoints =
        entries.count("ToneCurvePoints") ? parseToneCurvePoints(entries.at("ToneCurvePoints")) : PointList{};
    settings.sdrMaxPoint = migrateSdrMaxPoint(entries, settings.maxNits);

    const bool hasReferenceNits = entries.count("ReferenceNits") > 0;
    const bool hasSdrMaxPointKey = entries.count("SdrMaxPoint") > 0 || entries.count("MaxEndpointOutput") > 0;
    settings.referenceNits = hasReferenceNits ? std::stof(entries.at("ReferenceNits")) : legacyMidPoint;

    const std::vector<UserToneCurvePreset> *userPresets = fullConfig ? &fullConfig->userPresets : nullptr;

    if (!useToneCurve || !hasReferenceNits || !hasSdrMaxPointKey) {
        seedToneCurveFromLegacy(settings, legacyMidPoint);
    } else if (entries.count("ToneCurvePreset")) {
        settings.toneCurvePreset = presetFromString(entries.at("ToneCurvePreset"));
        settings.toneCurveUserPresetId =
            entries.count("ToneCurveUserPresetId") ? entries.at("ToneCurveUserPresetId") : std::string{};
        if (settings.toneCurvePreset != ToneCurvePreset::Custom) {
            applyToneCurvePreset(settings, userPresets);
        }
    } else {
        settings.toneCurvePreset = ToneCurvePreset::Custom;
    }

    return settings;
}

void writeCalibrationToEntries(std::unordered_map<std::string, std::string> &entries,
                               const CalibrationSettings &settings)
{
    entries["MaxNits"] = std::to_string(settings.maxNits);
    entries["GamutExpansion"] = std::to_string(settings.gamutExpansion);
    entries["BlackPoint"] = std::to_string(settings.blackPoint);
    entries["Vibrance"] = std::to_string(settings.vibrance);
    entries["ReferenceNits"] = std::to_string(static_cast<int>(std::lround(settings.referenceNits)));
    entries["SdrMaxPoint"] = formatSdrMaxPoint(settings.sdrMaxPoint);
    entries["ToneCurvePoints"] = formatToneCurvePoints(settings.toneCurvePoints);
    entries["ToneCurvePreset"] = presetToString(settings.toneCurvePreset);
    if (settings.toneCurvePreset == ToneCurvePreset::User) {
        entries["ToneCurveUserPresetId"] = settings.toneCurveUserPresetId;
    } else {
        entries.erase("ToneCurveUserPresetId");
    }
}

bool loadConfigFromFile(const std::string &path, ConfigData &out)
{
    IniStore store;
    if (!loadIniFile(path, store)) {
        return false;
    }

    out.general.autoActivateCalibrated = readBool(store, groupGeneral, "AutoActivateCalibrated", true);
    out.globalSettings = readCalibrationFromEntries(groupEntries(store, groupSettings),
                                                    readFloat(store, groupSettings, "MaxNits", 1000.0f), &out);

    const std::vector<std::string> presetIds = splitCommaList(readEntry(store, groupUserPresets, "PresetList"));
    out.userPresets.clear();
    out.userPresets.reserve(presetIds.size());
    for (const std::string &id : presetIds) {
        out.userPresets.push_back(loadUserPresetFromStore(store, id));
    }

    out.globalSettings = readCalibrationFromEntries(groupEntries(store, groupSettings),
                                                    readFloat(store, groupSettings, "MaxNits", 1000.0f), &out);

    out.appProfiles.clear();
    const std::vector<std::string> appKeys = splitCommaList(readEntry(store, groupApplications, "AppList"));
    for (const std::string &key : appKeys) {
        AppProfile profile;
        profile.metadata.key = key;
        const std::string group = appGroupName(key);
        profile.metadata.displayName = readEntry(store, group, "DisplayName", key);
        profile.metadata.windowClass = readEntry(store, group, "WindowClass");
        profile.metadata.resourceClass = readEntry(store, group, "ResourceClass");
        profile.metadata.desktopFile = readEntry(store, group, "DesktopFile");
        profile.metadata.steamAppId = readEntry(store, group, "SteamAppId");
        profile.metadata.autoActivate = readBool(store, group, "AutoActivate", true);
        profile.settings = readCalibrationFromEntries(groupEntries(store, group), profile.settings.maxNits, &out);
        out.appProfiles.push_back(std::move(profile));
    }

    return true;
}

bool saveGlobalSettingsToFile(const std::string &path, const CalibrationSettings &settings)
{
    IniStore store;
    loadIniFile(path, store);
    writeCalibrationToEntries(store[groupSettings], settings);
    return writeIniFile(path, store);
}

bool saveAppProfileToFile(const std::string &path, const AppProfile &profile)
{
    IniStore store;
    loadIniFile(path, store);

    std::vector<std::string> appList = splitCommaList(readEntry(store, groupApplications, "AppList"));
    if (std::find(appList.begin(), appList.end(), profile.metadata.key) == appList.end()) {
        appList.push_back(profile.metadata.key);
    }

    std::string listValue;
    for (size_t i = 0; i < appList.size(); ++i) {
        if (i > 0) {
            listValue.push_back(',');
        }
        listValue += appList[i];
    }
    store[groupApplications]["AppList"] = listValue;

    const std::string group = appGroupName(profile.metadata.key);
    store[group]["DisplayName"] = profile.metadata.displayName;
    store[group]["WindowClass"] = profile.metadata.windowClass;
    store[group]["ResourceClass"] = profile.metadata.resourceClass;
    store[group]["DesktopFile"] = profile.metadata.desktopFile;
    store[group]["SteamAppId"] = profile.metadata.steamAppId;
    store[group]["AutoActivate"] = profile.metadata.autoActivate ? "true" : "false";
    writeCalibrationToEntries(store[group], profile.settings);
    return writeIniFile(path, store);
}

std::optional<AppProfile> findAppProfile(const ConfigData &config, const std::string &key)
{
    for (const AppProfile &profile : config.appProfiles) {
        if (profile.metadata.key == key) {
            return profile;
        }
    }
    return std::nullopt;
}

std::string findAppKeyForIdentifiers(const ConfigData &config, const std::string &desktopFile,
                                     const std::string &resourceClass, const std::string &windowClass,
                                     const std::string &steamAppId)
{
    for (const AppProfile &profile : config.appProfiles) {
        if (identifiersMatch(profile.metadata, desktopFile, resourceClass, windowClass, steamAppId)) {
            return profile.metadata.key;
        }
    }
    return {};
}

CalibrationSettings resolveSettings(const ConfigData &config, const RuntimeContext &ctx, float maxDisplayNits)
{
    CalibrationSettings settings = config.globalSettings;

    if (!ctx.profileOverride.empty()) {
        if (const auto profile = findAppProfile(config, ctx.profileOverride)) {
            settings = profile->settings;
        }
    } else if (!ctx.steamAppId.empty()) {
        const std::string key = findAppKeyForIdentifiers(config, {}, {}, {}, ctx.steamAppId);
        if (!key.empty()) {
            if (const auto profile = findAppProfile(config, key)) {
                settings = profile->settings;
            }
        } else if (const auto profile = findAppProfile(config, steamAppProfileKey(ctx.steamAppId))) {
            settings = profile->settings;
        }
    } else if (!ctx.executableName.empty()) {
        const std::string key = findAppKeyForIdentifiers(config, {}, {}, ctx.executableName, {});
        if (!key.empty()) {
            if (const auto profile = findAppProfile(config, key)) {
                settings = profile->settings;
            }
        }
    }

    sanitizeCalibrationSettings(settings, maxDisplayNits, &config.userPresets);
    return settings;
}

} // namespace AutoHdrCoreCore
