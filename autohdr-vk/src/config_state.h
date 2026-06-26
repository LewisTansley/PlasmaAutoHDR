#pragma once

#include <autohdr/autohdr.h>

#include <mutex>
#include <string>

namespace autohdr_vk {

enum class ColorMode {
    SdrSrgb = 0,
    ScRgb = 1,
    Pq = 2,
};

struct PushConstants {
    float blackPoint = 0.0f;
    float colorVibrance = 0.0f;
    float gamutExpansion = 0.0f;
    float referenceNits = 203.0f;
    float displayPeak = 1000.0f;
    float toneCurveInputSpan = 203.0f;
    int colorMode = 0;
};

struct RuntimeState {
    AutoHdrCore::GpuUniforms uniforms{};
    ColorMode colorMode = ColorMode::SdrSrgb;
    std::string activeProfile;
    bool enabled = true;
};

class ConfigState {
public:
    void reload();
    RuntimeState snapshot() const;

private:
    mutable std::mutex mutex_;
    RuntimeState state_;
    AutoHdrCore::RuntimeContext buildContext() const;
};

} // namespace autohdr_vk
