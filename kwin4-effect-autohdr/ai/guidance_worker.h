/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "guidance_inference.h"

#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <memory>
#include <vector>

namespace AutoHdr {

class GuidanceWorker : public QThread
{
    Q_OBJECT

public:
    explicit GuidanceWorker(GuidanceInference *inference, QObject *parent = nullptr);
    ~GuidanceWorker() override;

    void submit(const float *inputRgb, int width, int height);
    bool tryTakeResult(std::vector<float> *outputRgba, int *width, int *height);
    bool takeFailedJob();
    void shutdown();

protected:
    void run() override;

private:
    GuidanceInference *m_inference = nullptr;
    QMutex m_mutex;
    QWaitCondition m_workAvailable;
    QWaitCondition m_workDone;
    std::atomic<bool> m_shutdown{false};
    bool m_hasWork = false;
    bool m_resultReady = false;
    bool m_jobFailed = false;
    int m_jobWidth = 0;
    int m_jobHeight = 0;
    std::vector<float> m_inputRgb;
    std::vector<float> m_outputRgba;
};

} // namespace AutoHdr
