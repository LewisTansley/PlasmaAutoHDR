/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "status_toast_overlay.h"

#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kToastCornerRadius = 8;
constexpr int kToastPaddingH = 14;
constexpr int kToastPaddingV = 8;
} // namespace

StatusToastOverlay::StatusToastOverlay(QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool | Qt::WindowTransparentForInput)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(kToastPaddingH, kToastPaddingV, kToastPaddingH, kToastPaddingV);
    layout->setSpacing(0);

    m_label = new QLabel(this);
    m_label->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_label->setStyleSheet(QStringLiteral("color: #ffffff; background: transparent;"));
    layout->addWidget(m_label);

    adjustSize();

    m_hideTimer.setSingleShot(true);
    connect(&m_hideTimer, &QTimer::timeout, this, &StatusToastOverlay::expired);
}

void StatusToastOverlay::setMessage(const QString &text)
{
    m_label->setText(text);
    adjustSize();
}

void StatusToastOverlay::showFor(int durationMs)
{
    show();
    m_hideTimer.start(durationMs);
}

QRect StatusToastOverlay::panelBlurRegion() const
{
    return rect();
}

void StatusToastOverlay::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath fillPath;
    fillPath.addRoundedRect(rect(), kToastCornerRadius, kToastCornerRadius);
    painter.fillPath(fillPath, QColor(18, 18, 18, 150));
}
