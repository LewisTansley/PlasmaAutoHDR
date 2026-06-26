#include "autohdr/config.h"
#include "autohdr/display_limits.h"
#include "autohdr/tone_curve.h"
#include "autohdr/tone_curve_presets.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {

bool nearlyEqual(float a, float b, float epsilon = 0.01f)
{
    return std::abs(a - b) <= epsilon;
}

} // namespace

int main()
{
    AutoHdrCore::CalibrationSettings settings;
    settings.referenceNits = 203.0f;
    settings.maxNits = 1000.0f;
    settings.toneCurvePreset = AutoHdrCore::ToneCurvePreset::SCurve;
    AutoHdrCore::applyToneCurvePreset(settings, nullptr);
    AutoHdrCore::sanitizeCalibrationSettings(settings, 1000.0f, nullptr);

    const AutoHdrCore::GpuUniforms uniforms = AutoHdrCore::buildGpuUniforms(settings, 1000.0f, nullptr);

    if (uniforms.toneCurveLut[0] > 1.0f) {
        std::cerr << "LUT start should be near zero\n";
        return 1;
    }
    if (uniforms.toneCurveLut[255] < uniforms.referenceNits) {
        std::cerr << "LUT end should reach peak range\n";
        return 1;
    }
    if (!nearlyEqual(uniforms.referenceNits, 203.0f)) {
        std::cerr << "Unexpected reference nits\n";
        return 1;
    }

    AutoHdrCore::ConfigData config;
    config.globalSettings = settings;
    AutoHdrCore::RuntimeContext ctx;
    ctx.steamAppId = "730";
    const AutoHdrCore::CalibrationSettings resolved =
        AutoHdrCore::resolveSettings(config, ctx, AutoHdrCore::readDisplayLimitsFromEnv().maxNits);
    if (!nearlyEqual(resolved.referenceNits, settings.referenceNits)) {
        std::cerr << "Global fallback resolution failed\n";
        return 1;
    }

    std::cout << "autohdr-core tests passed\n";
    return 0;
}
