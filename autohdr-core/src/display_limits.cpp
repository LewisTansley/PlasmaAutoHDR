#include "autohdr/display_limits.h"

#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

namespace AutoHdrCore {

namespace {

std::string resolveHome(const std::string &homeDir)
{
    if (!homeDir.empty()) {
        return homeDir;
    }
    if (const char *env = std::getenv("HOME")) {
        return env;
    }
    return "/home/deck";
}

float parseFloat(const std::string &text, float fallback)
{
    try {
        return std::stof(text);
    } catch (...) {
        return fallback;
    }
}

int parseInt(const std::string &text, int fallback)
{
    try {
        return std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

void readKwinrcLimits(const std::string &homeDir, DisplayLimits &limits)
{
    std::ifstream file(resolveHome(homeDir) + "/.config/kwinrc");
    if (!file.is_open()) {
        return;
    }

    bool inHdrSection = false;
    std::string line;
    while (std::getline(file, line)) {
        if (line == "[Windows_HDR]") {
            inHdrSection = true;
            continue;
        }
        if (!line.empty() && line.front() == '[') {
            inHdrSection = false;
            continue;
        }
        if (!inHdrSection) {
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "Reference") {
            limits.referenceNits = std::max(1.0f, parseFloat(value, limits.referenceNits));
        } else if (key == "MaxLuminance") {
            limits.maxNits = std::max(limits.referenceNits + 1.0f, parseFloat(value, limits.maxNits));
        }
    }
}

void readOutputConfigLimits(const std::string &homeDir, DisplayLimits &limits)
{
    std::ifstream file(resolveHome(homeDir) + "/.config/kwinoutputconfig.json");
    if (!file.is_open()) {
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();

    const std::regex peakRegex(R"("maxPeakBrightnessOverride"\s*:\s*(\d+))");
    const std::regex hdrRegex(R"("highDynamicRange"\s*:\s*true)");

    std::sregex_iterator hdrIt(json.begin(), json.end(), hdrRegex);
    std::sregex_iterator end;
    if (hdrIt == end) {
        return;
    }

    for (std::sregex_iterator peakIt(json.begin(), json.end(), peakRegex); peakIt != end; ++peakIt) {
        const int peak = parseInt((*peakIt)[1].str(), 0);
        if (peak > static_cast<int>(limits.maxNits)) {
            limits.maxNits = static_cast<float>(peak);
        }
    }
}

} // namespace

DisplayLimits readDisplayLimits(const std::string &homeDir)
{
    DisplayLimits limits;
    readKwinrcLimits(homeDir, limits);
    readOutputConfigLimits(homeDir, limits);
    return limits;
}

DisplayLimits readDisplayLimitsFromEnv()
{
    DisplayLimits limits = readDisplayLimits({});

    if (const char *ref = std::getenv("AUTOHDR_REFERENCE_NITS")) {
        limits.referenceNits = std::max(1.0f, parseFloat(ref, limits.referenceNits));
    }
    if (const char *max = std::getenv("AUTOHDR_MAX_NITS")) {
        limits.maxNits = std::max(limits.referenceNits + 1.0f, parseFloat(max, limits.maxNits));
    }

    return limits;
}

} // namespace AutoHdrCoreCore
