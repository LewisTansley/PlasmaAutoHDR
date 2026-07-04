/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "guidance_inference.h"

#include <QFile>
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

void runCpuGuidance(const float *inputRgb, int width, int height, float *outputRgba)
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

    const auto quantFlat = [](float delta) {
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

    const auto smoothstep = [](float edge0, float edge1, float x) {
        const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1e-6f), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };

    static const int ox[8] = {1, -1, 0, 0, 1, -1, 1, -1};
    static const int oy[8] = {0, 0, 1, -1, 1, 1, -1, -1};

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float *center = at(x, y);
            const float centerLuma = luma(center);
            const float centerSat = sat(center);

            float grad = 0.0f;
            float varAccum = 0.0f;
            float flatAccum = 0.0f;
            for (int i = 0; i < 8; ++i) {
                const float neighborLuma = luma(at(x + ox[i], y + oy[i]));
                const float d = neighborLuma - centerLuma;
                grad += std::abs(d);
                varAccum += d * d;
                flatAccum += quantFlat(d);
            }
            grad *= 0.125f;
            const float variance = varAccum * 0.125f;
            const float flatness = flatAccum * 0.125f;

            const float highlightConf = std::clamp((centerLuma - 0.55f) / 0.40f, 0.0f, 1.0f);
            const float highlightLift = std::clamp((centerLuma - 0.45f) / 0.53f, 0.0f, 1.0f);
            const float shoulderBand = smoothstep(0.85f, 0.95f, centerLuma);
            const float shoulderDetail = shoulderBand * flatness;
            float confidence = std::max(highlightConf, shoulderDetail * 0.85f);
            float expansion = 1.0f + confidence * highlightLift * 0.75f;

            const float shadowCore = 1.0f - smoothstep(0.0f, 0.14f, centerLuma);
            const float shadowTrans = (1.0f - smoothstep(0.14f, 0.24f, centerLuma)) * 0.45f;
            const float shadowRegion = std::max(shadowCore, shadowTrans);
            const float lowVar = 1.0f - smoothstep(0.0f, 0.0025f, variance);
            const float softEdge = 1.0f - smoothstep(0.02f, 0.10f, grad);
            const float mildGrad =
                smoothstep(0.004f, 0.02f, grad) * (1.0f - smoothstep(0.06f, 0.16f, grad));
            const float shadowSoft = 0.65f + softEdge * 0.35f;
            float shadowMask =
                shadowRegion * std::max(flatness, std::max(lowVar * 0.85f, mildGrad * 1.15f)) * shadowSoft;

            const float midBand =
                smoothstep(0.05f, 0.15f, centerLuma) * (1.0f - smoothstep(0.70f, 0.88f, centerLuma));
            const float rampGrad =
                smoothstep(0.003f, 0.025f, grad) * (1.0f - smoothstep(0.08f, 0.18f, grad));
            const float shadowBand = (1.0f - smoothstep(0.0f, 0.12f, centerLuma)) * rampGrad;
            const float shoulderBandMask =
                smoothstep(0.82f, 0.94f, centerLuma) * flatness * (1.0f - smoothstep(0.08f, 0.18f, grad));
            const float regionBand =
                std::max(shadowBand, std::max(midBand, shoulderBandMask * 0.75f));
            float bandMask = regionBand * std::max(flatness, rampGrad * 0.7f) * softEdge;

            // flatPanel also matches sky/specular interiors — do not apply to highlight R/G.
            const float flatPanel = (1.0f - smoothstep(0.005f, 0.035f, grad))
                * std::clamp((centerLuma - 0.75f) / 0.23f, 0.0f, 1.0f)
                * (1.0f - std::clamp((centerSat - 0.05f) / 0.25f, 0.0f, 1.0f));
            const float textEdge = std::clamp((grad - 0.08f) / 0.17f, 0.0f, 1.0f)
                * std::clamp((centerLuma - 0.4f) / 0.5f, 0.0f, 1.0f);
            const float textKeep = 1.0f - textEdge * 0.85f;
            const float panelKeep = 1.0f - flatPanel;
            const float detailKeep = panelKeep * textKeep;

            confidence *= textKeep;
            expansion = 1.0f + (expansion - 1.0f) * textKeep;
            shadowMask *= detailKeep;
            bandMask *= detailKeep;

            const int idx = (y * width + x) * 4;
            outputRgba[idx + 0] = std::clamp(expansion, 1.0f, 1.75f);
            outputRgba[idx + 1] = std::clamp(confidence, 0.0f, 1.0f);
            outputRgba[idx + 2] = std::clamp(shadowMask, 0.0f, 1.0f);
            outputRgba[idx + 3] = std::clamp(bandMask, 0.0f, 1.0f);
        }
    }
}

class CpuGuidanceInference final : public GuidanceInference
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
        runCpuGuidance(inputRgb, width, height, outputRgba);
        return true;
    }
};

#if defined(AUTOHDR_HAS_ONNX)

QString preferredEpFromEnv()
{
    return qEnvironmentVariable("AUTOHDR_ONNX_EP").trimmed().toLower();
}

bool tryProvider(Ort::SessionOptions &options, const char *name, const QString &forcedEp)
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
    } catch (const Ort::Exception &) {
        return false;
    } catch (...) {
        return false;
    }
}

class OnnxGuidanceInference final : public GuidanceInference
{
public:
    explicit OnnxGuidanceInference(const QString &modelPath)
    {
        if (!QFileInfo::exists(modelPath)) {
            qWarning("AutoHDR ONNX: model not found: %s", qPrintable(modelPath));
            return;
        }

        try {
            m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "AutoHDR");
            Ort::SessionOptions options;
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            options.SetIntraOpNumThreads(1);

            const QString forcedEp = preferredEpFromEnv();
            // AMD-first: Vulkan, then CUDA (NVIDIA), then CPU default.
            if (tryProvider(options, "Vulkan", forcedEp)) {
                m_epName = QStringLiteral("vulkan");
            } else if (tryProvider(options, "CUDA", forcedEp)) {
                m_epName = QStringLiteral("cuda");
            } else {
                m_epName = QStringLiteral("cpu");
                if (!forcedEp.isEmpty() && forcedEp != QLatin1String("cpu")) {
                    qWarning("AutoHDR ONNX: requested EP '%s' unavailable, using CPU", qPrintable(forcedEp));
                }
            }

            const QByteArray pathBytes = QFile::encodeName(modelPath);
            m_session = std::make_unique<Ort::Session>(*m_env, pathBytes.constData(), options);
            m_memoryInfo = std::make_unique<Ort::MemoryInfo>(
                Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

            Ort::AllocatorWithDefaultOptions allocator;
            m_inputName = m_session->GetInputNameAllocated(0, allocator).get();
            m_outputName = m_session->GetOutputNameAllocated(0, allocator).get();
            m_available = true;
            qInfo("AutoHDR ONNX: loaded %s (EP=%s)", qPrintable(modelPath), qPrintable(m_epName));
        } catch (const Ort::Exception &ex) {
            qWarning("AutoHDR ONNX: session init failed: %s", ex.what());
            m_session.reset();
            m_memoryInfo.reset();
            m_available = false;
        }
    }

    bool isAvailable() const override
    {
        return m_available;
    }

    QString backendName() const override
    {
        return m_available ? QStringLiteral("onnx-%1").arg(m_epName) : QStringLiteral("onnx-unavailable");
    }

    bool run(const float *inputRgb, int width, int height, float *outputRgba) override
    {
        if (!m_available || !m_memoryInfo || !inputRgb || !outputRgba || width <= 0 || height <= 0) {
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
                // NCHW RGBA
                for (size_t i = 0; i < plane; ++i) {
                    outputRgba[i * 4 + 0] = outData[i];
                    outputRgba[i * 4 + 1] = outData[plane + i];
                    outputRgba[i * 4 + 2] = outData[plane * 2 + i];
                    outputRgba[i * 4 + 3] = outData[plane * 3 + i];
                }
            } else if (shape.size() == 4 && shape[1] == 2) {
                // Legacy RG guidance_v0: fill B/A with zeros.
                for (size_t i = 0; i < plane; ++i) {
                    outputRgba[i * 4 + 0] = outData[i];
                    outputRgba[i * 4 + 1] = outData[plane + i];
                    outputRgba[i * 4 + 2] = 0.0f;
                    outputRgba[i * 4 + 3] = 0.0f;
                }
            } else if (shape.size() == 4 && shape[3] == 4) {
                std::copy(outData, outData + plane * 4, outputRgba);
            } else if (shape.size() == 4 && shape[3] == 2) {
                for (size_t i = 0; i < plane; ++i) {
                    outputRgba[i * 4 + 0] = outData[i * 2 + 0];
                    outputRgba[i * 4 + 1] = outData[i * 2 + 1];
                    outputRgba[i * 4 + 2] = 0.0f;
                    outputRgba[i * 4 + 3] = 0.0f;
                }
            } else {
                runCpuGuidance(inputRgb, width, height, outputRgba);
            }
            return true;
        } catch (const Ort::Exception &ex) {
            qWarning("AutoHDR ONNX: run failed: %s", ex.what());
            runCpuGuidance(inputRgb, width, height, outputRgba);
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

#if defined(AUTOHDR_HAS_ONNX)

std::unique_ptr<GuidanceInference> createOnnxGuidanceInference(const QString &modelPath)
{
    auto onnx = std::make_unique<OnnxGuidanceInference>(modelPath);
    if (onnx->isAvailable()) {
        return onnx;
    }
    return std::make_unique<CpuGuidanceInference>();
}

bool onnxGuidanceRuntimeBuilt()
{
    return true;
}

#else // !AUTOHDR_HAS_ONNX

std::unique_ptr<GuidanceInference> createOnnxGuidanceInference(const QString &modelPath)
{
    Q_UNUSED(modelPath)
    return std::make_unique<CpuGuidanceInference>();
}

bool onnxGuidanceRuntimeBuilt()
{
    return false;
}

#endif

} // namespace AutoHdr
