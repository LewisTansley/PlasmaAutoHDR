/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "chroma_inference.h"

#include <QFileInfo>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(AUTOHDR_HAS_ONNX)
#include <onnxruntime_cxx_api.h>
#endif

namespace AutoHdr {

namespace {

constexpr float kSrgbToAp1[3][3] = {
    {0.616850994f, 0.069866394f, 0.020549067f},
    {0.334062934f, 0.917416679f, 0.107642211f},
    {0.049086072f, 0.012716927f, 0.871808722f},
};

constexpr float kAp1ToXyz[3][3] = {
    {0.647507191f, 0.266086400f, -0.005448868f},
    {0.134379134f, 0.675967813f, 0.004072095f},
    {0.168569595f, 0.057945795f, 1.090434551f},
};

void ap1ChromaOf(const float *rgb, float *chromaOut)
{
    float ap1[3] = {
        kSrgbToAp1[0][0] * rgb[0] + kSrgbToAp1[0][1] * rgb[1] + kSrgbToAp1[0][2] * rgb[2],
        kSrgbToAp1[1][0] * rgb[0] + kSrgbToAp1[1][1] * rgb[1] + kSrgbToAp1[1][2] * rgb[2],
        kSrgbToAp1[2][0] * rgb[0] + kSrgbToAp1[2][1] * rgb[1] + kSrgbToAp1[2][2] * rgb[2],
    };
    const float y = std::max(
        kAp1ToXyz[1][0] * ap1[0] + kAp1ToXyz[1][1] * ap1[1] + kAp1ToXyz[1][2] * ap1[2], 1e-6f);
    chromaOut[0] = ap1[0] / y;
    chromaOut[1] = ap1[1] / y;
    chromaOut[2] = ap1[2] / y;
}

void runCpuChroma(const float *inputRgb, int width, int height, float *outputRgba)
{
    const auto at = [&](int x, int y) -> const float * {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return inputRgb + (y * width + x) * 3;
    };

    const auto luma = [](const float *rgb) {
        return rgb[0] * 0.2126f + rgb[1] * 0.7152f + rgb[2] * 0.0722f;
    };

    const auto sat = [](const float *rgb) {
        const float mx = std::max(rgb[0], std::max(rgb[1], rgb[2]));
        const float mn = std::min(rgb[0], std::min(rgb[1], rgb[2]));
        return (mx - mn) / std::max(mx, 1e-4f);
    };

    const auto smoothstep = [](float edge0, float edge1, float x) {
        const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1e-6f), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };

    const auto quantFlatChroma = [](float delta) {
        constexpr float s = 1.0f / 255.0f;
        const float d = std::abs(delta);
        if (d <= s + 1.0e-4f) {
            return 1.0f;
        }
        if (d >= s * 2.0f) {
            return 0.0f;
        }
        return 1.0f - (d - (s + 1.0e-4f)) / (s * 2.0f - (s + 1.0e-4f));
    };

    static const int ox[8] = {1, -1, 0, 0, 1, -1, 1, -1};
    static const int oy[8] = {0, 0, 1, -1, 1, 1, -1, -1};

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float *center = at(x, y);
            const float centerLuma = luma(center);
            const float centerSat = sat(center);
            float centerChroma[3];
            ap1ChromaOf(center, centerChroma);

            float lumaGrad = 0.0f;
            float chromaFlat = 0.0f;
            float chromaVar = 0.0f;
            float avgChroma[3] = {centerChroma[0], centerChroma[1], centerChroma[2]};

            for (int i = 0; i < 8; ++i) {
                const float *n = at(x + ox[i], y + oy[i]);
                const float nLuma = luma(n);
                lumaGrad += std::abs(nLuma - centerLuma);
                float nChroma[3];
                ap1ChromaOf(n, nChroma);
                const float dr = nChroma[0] - centerChroma[0];
                const float dg = nChroma[1] - centerChroma[1];
                const float db = nChroma[2] - centerChroma[2];
                chromaVar += dr * dr + dg * dg + db * db;
                chromaFlat += quantFlatChroma(dr) * quantFlatChroma(dg) * quantFlatChroma(db);
                avgChroma[0] += nChroma[0];
                avgChroma[1] += nChroma[1];
                avgChroma[2] += nChroma[2];
            }
            lumaGrad *= 0.125f;
            chromaVar *= 0.125f;
            chromaFlat *= 0.125f;
            avgChroma[0] /= 9.0f;
            avgChroma[1] /= 9.0f;
            avgChroma[2] /= 9.0f;

            const float midRegion =
                smoothstep(0.08f, 0.18f, centerLuma) * (1.0f - smoothstep(0.82f, 0.92f, centerLuma));
            const float smoothSky = 1.0f - smoothstep(0.012f, 0.06f, lumaGrad);
            const float lowChromaVar = 1.0f - smoothstep(0.0002f, 0.002f, chromaVar);
            float strength = midRegion * std::max(chromaFlat, lowChromaVar * 0.75f) * smoothSky;

            const float centerDist = std::sqrt((centerChroma[0] - 1.0f) * (centerChroma[0] - 1.0f)
                + (centerChroma[1] - 1.0f) * (centerChroma[1] - 1.0f));
            const float avgDist = std::sqrt((avgChroma[0] - 1.0f) * (avgChroma[0] - 1.0f)
                + (avgChroma[1] - 1.0f) * (avgChroma[1] - 1.0f));
            const float satHint = std::clamp(avgDist - centerDist, -0.5f, 0.5f);
            const float satStored = satHint * 0.5f + 0.5f;

            const float cAngle = std::atan2(centerChroma[1] - 1.0f, centerChroma[0] - 1.0f);
            const float aAngle = std::atan2(avgChroma[1] - 1.0f, avgChroma[0] - 1.0f);
            const float hueHint = std::clamp(aAngle - cAngle, -0.15f, 0.15f) / 0.15f;
            const float hueStored = hueHint * 0.5f + 0.5f;

            const float textEdge = std::clamp((lumaGrad - 0.08f) / 0.17f, 0.0f, 1.0f)
                * std::clamp((centerLuma - 0.4f) / 0.5f, 0.0f, 1.0f);
            const float neutralGray = 1.0f - smoothstep(0.02f, 0.12f, centerSat);
            const float skinHue =
                smoothstep(0.02f, 0.08f, centerSat) * (1.0f - smoothstep(0.45f, 0.65f, centerSat));
            const float skinGate = skinHue * (1.0f - smoothstep(0.15f, 0.35f, lumaGrad));
            const float preserveChroma = std::max(std::max(textEdge * 0.85f, neutralGray * 0.5f), skinGate * 0.7f);
            const float localColorIntensity = 0.55f + (0.15f - 0.55f) * preserveChroma;

            const float highlightBand = smoothstep(0.88f, 0.98f, centerLuma);
            strength = std::max(strength, highlightBand * (1.0f - textEdge) * 0.65f);
            strength *= 1.0f - textEdge * 0.9f;

            const int idx = (y * width + x) * 4;
            outputRgba[idx + 0] = std::clamp(strength, 0.0f, 1.0f);
            outputRgba[idx + 1] = std::clamp(satStored, 0.0f, 1.0f);
            outputRgba[idx + 2] = std::clamp(hueStored, 0.0f, 1.0f);
            outputRgba[idx + 3] = std::clamp(localColorIntensity, 0.0f, 1.0f);
        }
    }
}

class CpuChromaInference final : public ChromaInference
{
public:
    bool isAvailable() const override
    {
        return true;
    }

    QString backendName() const override
    {
        return QStringLiteral("cpu-reference");
    }

    bool run(const float *inputRgb, int width, int height, float *outputRgba) override
    {
        if (!inputRgb || !outputRgba || width <= 0 || height <= 0) {
            return false;
        }
        runCpuChroma(inputRgb, width, height, outputRgba);
        return true;
    }
};

#if defined(AUTOHDR_HAS_ONNX)

QString chromaPreferredEpFromEnv()
{
    return qEnvironmentVariable("AUTOHDR_ONNX_EP").trimmed().toLower();
}

bool chromaTryProvider(Ort::SessionOptions &options, const char *name, const QString &forcedEp)
{
    const QString provider = QString::fromLatin1(name).toLower();
    if (!forcedEp.isEmpty() && forcedEp != provider) {
        return false;
    }
    try {
        std::unordered_map<std::string, std::string> providerOptions;
        providerOptions["device_id"] = "0";
        options.AppendExecutionProvider(name, providerOptions);
        return true;
    } catch (...) {
        return false;
    }
}

class OnnxChromaInference final : public ChromaInference
{
public:
    explicit OnnxChromaInference(const QString &modelPath)
    {
        if (!QFileInfo::exists(modelPath)) {
            qWarning("AutoHDR Chroma ONNX: model not found: %s", qPrintable(modelPath));
            return;
        }

        try {
            m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "AutoHDRChroma");
            Ort::SessionOptions options;
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            options.SetIntraOpNumThreads(1);

            const QString forcedEp = chromaPreferredEpFromEnv();
            if (chromaTryProvider(options, "Vulkan", forcedEp)) {
                m_epName = QStringLiteral("vulkan");
            } else if (chromaTryProvider(options, "CUDA", forcedEp)) {
                m_epName = QStringLiteral("cuda");
            } else {
                m_epName = QStringLiteral("cpu");
            }

            const QByteArray pathBytes = QFile::encodeName(modelPath);
            m_session = std::make_unique<Ort::Session>(*m_env, pathBytes.constData(), options);
            m_memoryInfo = std::make_unique<Ort::MemoryInfo>(
                Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

            Ort::AllocatorWithDefaultOptions allocator;
            m_inputName = m_session->GetInputNameAllocated(0, allocator).get();
            m_outputName = m_session->GetOutputNameAllocated(0, allocator).get();
            m_available = true;
            qInfo("AutoHDR Chroma ONNX: loaded %s (EP=%s)", qPrintable(modelPath), qPrintable(m_epName));
        } catch (const Ort::Exception &ex) {
            qWarning("AutoHDR Chroma ONNX: session init failed: %s", ex.what());
            m_available = false;
        }
    }

    bool isAvailable() const override
    {
        return m_available;
    }

    QString backendName() const override
    {
        return m_available ? QStringLiteral("onnx-chroma-%1").arg(m_epName) : QStringLiteral("onnx-chroma-unavailable");
    }

    bool run(const float *inputRgb, int width, int height, float *outputRgba) override
    {
        if (!m_available || !inputRgb || !outputRgba || width <= 0 || height <= 0) {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        try {
            const std::array<int64_t, 4> inputShape = {1, 3, height, width};
            const size_t plane = static_cast<size_t>(width) * static_cast<size_t>(height);
            m_nchw.resize(plane * 3);
            for (size_t i = 0; i < plane; ++i) {
                m_nchw[i] = inputRgb[i * 3 + 0];
                m_nchw[plane + i] = inputRgb[i * 3 + 1];
                m_nchw[plane * 2 + i] = inputRgb[i * 3 + 2];
            }

            Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                *m_memoryInfo, m_nchw.data(), m_nchw.size(), inputShape.data(), inputShape.size());

            const char *inputNames[] = {m_inputName.c_str()};
            const char *outputNames[] = {m_outputName.c_str()};
            auto outputs = m_session->Run(Ort::RunOptions{nullptr}, inputNames, &inputTensor, 1, outputNames, 1);
            float *outData = outputs.front().GetTensorMutableData<float>();

            const auto shape = outputs.front().GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() == 4 && shape[1] == 4) {
                for (size_t i = 0; i < plane; ++i) {
                    outputRgba[i * 4 + 0] = outData[i];
                    outputRgba[i * 4 + 1] = outData[plane + i];
                    outputRgba[i * 4 + 2] = outData[plane * 2 + i];
                    outputRgba[i * 4 + 3] = outData[plane * 3 + i];
                }
            } else {
                runCpuChroma(inputRgb, width, height, outputRgba);
            }
            return true;
        } catch (const Ort::Exception &ex) {
            qWarning("AutoHDR Chroma ONNX: run failed: %s", ex.what());
            runCpuChroma(inputRgb, width, height, outputRgba);
            return true;
        }
    }

private:
    std::unique_ptr<Ort::Env> m_env;
    std::unique_ptr<Ort::Session> m_session;
    std::unique_ptr<Ort::MemoryInfo> m_memoryInfo;
    std::string m_inputName;
    std::string m_outputName;
    std::vector<float> m_nchw;
    QString m_epName = QStringLiteral("cpu");
    bool m_available = false;
    std::mutex m_mutex;
};

#endif // AUTOHDR_HAS_ONNX

} // namespace

std::unique_ptr<ChromaInference> createCpuChromaInference()
{
    return std::make_unique<CpuChromaInference>();
}

#if defined(AUTOHDR_HAS_ONNX)

std::unique_ptr<ChromaInference> createOnnxChromaInference(const QString &modelPath)
{
    auto onnx = std::make_unique<OnnxChromaInference>(modelPath);
    if (onnx->isAvailable()) {
        return onnx;
    }
    return createCpuChromaInference();
}

bool onnxChromaRuntimeBuilt()
{
    return true;
}

#else

std::unique_ptr<ChromaInference> createOnnxChromaInference(const QString &modelPath)
{
    Q_UNUSED(modelPath)
    return createCpuChromaInference();
}

bool onnxChromaRuntimeBuilt()
{
    return false;
}

#endif

} // namespace AutoHdr
