/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QString>
#include <memory>

namespace AutoHdr {

class ChromaInference
{
public:
    virtual ~ChromaInference() = default;

    virtual bool isAvailable() const = 0;
    virtual QString backendName() const = 0;

    // inputRgb: interleaved RGB float32, row-major, values in [0, 1+] linear-relative.
    // outputRgba: interleaved RGBA float32:
    //   R = chroma correction strength [0,1]
    //   G = saturation residual hint (0..1, remapped to -1..1 in shader)
    //   B = hue residual hint (0..1, remapped to -1..1 in shader)
    //   A = per-pixel colorIntensity blend [0,1]
    virtual bool run(const float *inputRgb, int width, int height, float *outputRgba) = 0;
};

std::unique_ptr<ChromaInference> createCpuChromaInference();
std::unique_ptr<ChromaInference> createOnnxChromaInference(const QString &modelPath);
bool onnxChromaRuntimeBuilt();

} // namespace AutoHdr
