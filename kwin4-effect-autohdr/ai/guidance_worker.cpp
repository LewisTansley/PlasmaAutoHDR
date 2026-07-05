/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "guidance_worker.h"

namespace AutoHdr {

GuidanceWorker::GuidanceWorker(GuidanceInference *inference, QObject *parent)
    : QThread(parent)
    , m_inference(inference)
{
}

GuidanceWorker::~GuidanceWorker()
{
    shutdown();
}

void GuidanceWorker::shutdown()
{
    {
        QMutexLocker lock(&m_mutex);
        m_shutdown = true;
        m_workAvailable.wakeAll();
    }
    if (isRunning()) {
        wait(5000);
    }
}

uint64_t GuidanceWorker::submit(const float *inputRgb, int width, int height)
{
    if (!inputRgb || width <= 0 || height <= 0 || !m_inference) {
        return 0;
    }

    QMutexLocker lock(&m_mutex);
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    m_inputRgb.resize(pixelCount * 3);
    std::copy(inputRgb, inputRgb + pixelCount * 3, m_inputRgb.begin());
    m_jobWidth = width;
    m_jobHeight = height;
    ++m_submitGeneration;
    m_hasWork = true;
    m_resultReady = false;
    m_jobFailed = false;
    m_workAvailable.wakeOne();
    return m_submitGeneration;
}

bool GuidanceWorker::takeFailedJob()
{
    QMutexLocker lock(&m_mutex);
    if (!m_jobFailed) {
        return false;
    }
    m_jobFailed = false;
    return true;
}

bool GuidanceWorker::hasInflightWork()
{
    QMutexLocker lock(&m_mutex);
    return m_hasWork || m_processing;
}

bool GuidanceWorker::tryTakeResult(std::vector<float> *outputRgba, int *width, int *height,
                                   uint64_t *resultGeneration)
{
    if (!outputRgba) {
        return false;
    }

    QMutexLocker lock(&m_mutex);
    if (!m_resultReady) {
        return false;
    }
    *outputRgba = m_outputRgba;
    if (width) {
        *width = m_jobWidth;
    }
    if (height) {
        *height = m_jobHeight;
    }
    if (resultGeneration) {
        *resultGeneration = m_resultGeneration;
    }
    m_resultReady = false;
    return true;
}

void GuidanceWorker::run()
{
    while (true) {
        QMutexLocker lock(&m_mutex);
        while (!m_hasWork && !m_shutdown) {
            m_workAvailable.wait(&m_mutex);
        }
        if (m_shutdown) {
            break;
        }

        const int width = m_jobWidth;
        const int height = m_jobHeight;
        std::vector<float> input = m_inputRgb;
        const uint64_t genAtStart = m_submitGeneration;
        m_hasWork = false;
        m_processing = true;
        lock.unlock();

        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
        std::vector<float> output(pixelCount * 4);
        const bool ok = m_inference && m_inference->run(input.data(), width, height, output.data());

        lock.relock();
        m_processing = false;
        if (genAtStart != m_submitGeneration) {
            // A newer frame arrived during inference — discard stale output.
            continue;
        }
        if (ok) {
            m_outputRgba = std::move(output);
            m_resultGeneration = genAtStart;
            m_resultReady = true;
        } else {
            m_failedGeneration = genAtStart;
            m_jobFailed = true;
        }
        m_workDone.wakeAll();
    }
}

} // namespace AutoHdr
