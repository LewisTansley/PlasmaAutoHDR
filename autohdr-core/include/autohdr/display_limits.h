#pragma once

#include "types.h"

#include <string>

namespace AutoHdrCore {

DisplayLimits readDisplayLimits(const std::string &homeDir = {});
DisplayLimits readDisplayLimitsFromEnv();

} // namespace AutoHdrCoreCore
