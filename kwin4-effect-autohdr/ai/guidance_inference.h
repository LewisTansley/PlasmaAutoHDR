/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QString>
#include <memory>
#include <vector>

namespace AutoHdr {

class GuidanceInference
{
public:
    virtual ~GuidanceInference() = default;

    virtual bool isAvailable() const = 0;
    virtual QString backendName() const = 0;

    // inputRgb: interleaved RGB float32, row-major, values in [0, 1+] linear-relative.
    // outputRgba: interleaved RGBA float32:
    //   R = highlight expansion (>=1)
    //   G = highlight confidence / shoulder-detail mask [0,1]
    //   B = shadow detail mask [0,1]
    //   A = depth / local-contrast mask [0,1]
    virtual bool run(const float *inputRgb, int width, int height, float *outputRgba) = 0;
};

std::unique_ptr<GuidanceInference> createOnnxGuidanceInference(const QString &modelPath);
bool onnxGuidanceRuntimeBuilt();

} // namespace AutoHdr
