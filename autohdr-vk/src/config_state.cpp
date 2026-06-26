#include "config_state.h"

#include <cstdlib>
#include <unistd.h>

namespace autohdr_vk {

namespace {

ColorMode detectColorMode()
{
    if (const char *dxvkHdr = std::getenv("DXVK_HDR")) {
        if (dxvkHdr[0] == '1') {
            return ColorMode::ScRgb;
        }
    }
    if (const char *mode = std::getenv("AUTOHDR_VK_COLOR_MODE")) {
        if (mode[0] == '2') {
            return ColorMode::Pq;
        }
        if (mode[0] == '1') {
            return ColorMode::ScRgb;
        }
    }
    return ColorMode::SdrSrgb;
}

std::string readEnvString(const char *name)
{
    if (const char *value = std::getenv(name)) {
        return value;
    }
    return {};
}

std::string executableBasename()
{
    char buffer[4096];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return {};
    }
    buffer[length] = '\0';
    const std::string path(buffer);
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

AutoHdrCore::RuntimeContext ConfigState::buildContext() const
{
    AutoHdrCore::RuntimeContext ctx;
    ctx.steamAppId = readEnvString("SteamAppId");
    ctx.profileOverride = readEnvString("AUTOHDR_VK_PROFILE");
    ctx.executableName = executableBasename();
    return ctx;
}

void ConfigState::reload()
{
    RuntimeState next;
    next.enabled = std::getenv("DISABLE_AUTOHDR_VK") == nullptr;
    next.colorMode = detectColorMode();

    AutoHdrCore::ConfigData config;
    const std::string configPath = AutoHdrCore::defaultConfigPath();
    if (!AutoHdrCore::loadConfigFromFile(configPath, config)) {
        config.globalSettings.referenceNits = 203.0f;
        config.globalSettings.maxNits = 1000.0f;
    }

    const AutoHdrCore::DisplayLimits limits = AutoHdrCore::readDisplayLimitsFromEnv();
    const AutoHdrCore::RuntimeContext ctx = buildContext();
    const AutoHdrCore::CalibrationSettings settings =
        AutoHdrCore::resolveSettings(config, ctx, limits.maxNits);

    next.uniforms = AutoHdrCore::buildGpuUniforms(settings, limits.maxNits, &config.userPresets);
    next.uniforms.referenceNits = limits.referenceNits > 1.0f ? limits.referenceNits : next.uniforms.referenceNits;

    if (!ctx.profileOverride.empty()) {
        next.activeProfile = ctx.profileOverride;
    } else if (!ctx.steamAppId.empty()) {
        next.activeProfile = AutoHdrCore::steamAppProfileKey(ctx.steamAppId);
    } else {
        next.activeProfile = "global";
    }

    std::lock_guard lock(mutex_);
    state_ = std::move(next);
}

RuntimeState ConfigState::snapshot() const
{
    std::lock_guard lock(mutex_);
    return state_;
}

} // namespace autohdr_vk
