/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QTimer>
#include <QWidget>

class QLabel;

class StatusToastOverlay : public QWidget
{
    Q_OBJECT

public:
    explicit StatusToastOverlay(QWidget *parent = nullptr);

    void setMessage(const QString &text);
    void showFor(int durationMs = 5000);
    QRect panelBlurRegion() const;

Q_SIGNALS:
    void expired();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QLabel *m_label = nullptr;
    QTimer m_hideTimer;
};
