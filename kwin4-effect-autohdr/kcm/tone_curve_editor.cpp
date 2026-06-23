/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "tone_curve_editor.h"

#include "tone_curve.h"

#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

constexpr int kPlotSize = 280;

} // namespace

ToneCurveEditor::ToneCurveEditor(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(kPlotSize + 40, kPlotSize + 120);

    auto *layout = new QVBoxLayout(this);

    auto *inputsLayout = new QHBoxLayout();
    m_peakNits = new QSpinBox(this);
    m_peakNits->setRange(1, 10000);
    m_peakNits->setSingleStep(10);
    m_referenceNits = new QSpinBox(this);
    m_referenceNits->setRange(static_cast<int>(AutoHdr::kReferenceNitsMin),
                              static_cast<int>(AutoHdr::kReferenceNitsMax));
    m_referenceNits->setSingleStep(1);
    inputsLayout->addWidget(new QLabel(tr("Peak nits:"), this));
    inputsLayout->addWidget(m_peakNits);
    inputsLayout->addSpacing(12);
    inputsLayout->addWidget(new QLabel(tr("Reference nits:"), this));
    inputsLayout->addWidget(m_referenceNits);
    inputsLayout->addStretch();
    layout->addLayout(inputsLayout);

    auto *readoutLayout = new QHBoxLayout();
    m_inputLabel = new QLabel(tr("Input: —"), this);
    m_outputLabel = new QLabel(tr("Output: —"), this);
    readoutLayout->addWidget(m_inputLabel);
    readoutLayout->addSpacing(16);
    readoutLayout->addWidget(m_outputLabel);
    readoutLayout->addStretch();
    layout->addLayout(readoutLayout);

    layout->addStretch();

    connect(m_peakNits, qOverload<int>(&QSpinBox::valueChanged), this, [this]() {
        if (m_blockSignals) {
            return;
        }
        const float previousPeak = m_peakNitsValue;
        m_peakNitsValue = static_cast<float>(m_peakNits->value());
        if (qAbs(previousPeak - m_peakNitsValue) > 1e-3f) {
            maybeSnapSdrMaxToPeak(previousPeak);
        }
        syncSpinRanges();
        sanitizePoints();
        update();
        Q_EMIT settingsChanged();
    });
    connect(m_peakNits, &QSpinBox::editingFinished, this, [this]() { emitChanged(true); });
    connect(m_referenceNits, qOverload<int>(&QSpinBox::valueChanged), this, [this]() {
        if (m_blockSignals) {
            return;
        }
        m_peakNits->interpretText();
        m_peakNitsValue = static_cast<float>(m_peakNits->value());
        m_referenceNitsValue =
            AutoHdr::clampReferenceNits(static_cast<float>(m_referenceNits->value()));
        enforcePeakFloor();
        sanitizePoints();
        update();
        Q_EMIT settingsChanged();
    });
    connect(m_referenceNits, &QSpinBox::editingFinished, this, [this]() { emitChanged(true); });
}

void ToneCurveEditor::setHdrLimits(int minPeakNits, int maxDisplayNits)
{
    m_minPeakNits = minPeakNits;
    m_maxDisplayNits = maxDisplayNits;
    syncSpinRanges();
}

void ToneCurveEditor::setValues(float peakNits, float referenceNits, const QPointF &sdrMaxPoint,
                                const QVector<QPointF> &intermediatePoints)
{
    m_blockSignals = true;
    m_peakNitsValue = peakNits;
    m_referenceNitsValue = referenceNits;
    m_sdrMaxPoint = sdrMaxPoint;
    m_intermediatePoints = intermediatePoints;
    syncSpinRanges();
    m_peakNits->setValue(qRound(peakNits));
    m_referenceNits->setValue(qRound(referenceNits));
    enforcePeakFloor();
    sanitizePoints();
    m_blockSignals = false;
    update();
}

void ToneCurveEditor::getValues(float &peakNits, float &referenceNits, QPointF &sdrMaxPoint,
                                QVector<QPointF> &intermediatePoints)
{
    flushSpinValuesFromUi();
    peakNits = m_peakNitsValue;
    referenceNits = m_referenceNitsValue;
    sdrMaxPoint = m_sdrMaxPoint;
    intermediatePoints = m_intermediatePoints;
}

void ToneCurveEditor::seedFromLegacy(float midPointNits, float peakNits)
{
    setValues(peakNits, midPointNits, QPointF(midPointNits, peakNits), {});
}

QRectF ToneCurveEditor::plotRect() const
{
    return QRectF(20.0, 60.0, kPlotSize, kPlotSize);
}

QRectF ToneCurveEditor::interactiveRect() const
{
    return plotRect().adjusted(-kHitPadding, -kHitPadding, kHitPadding, kHitPadding);
}

QPointF ToneCurveEditor::nitsToPixel(float inputNits, float outputNits) const
{
    const QRectF plot = plotRect();
    const float inputSpan = qMax(m_referenceNitsValue, 1.0f);
    const float outputSpan = qMax(m_peakNitsValue, 1.0f);
    const float x = inputNits / inputSpan;
    const float y = outputNits / outputSpan;
    return QPointF(plot.left() + x * plot.width(), plot.bottom() - y * plot.height());
}

QPointF ToneCurveEditor::pixelToNits(const QPointF &pixel) const
{
    const QRectF plot = plotRect();
    const float inputSpan = qMax(m_referenceNitsValue, 1.0f);
    const float outputSpan = qMax(m_peakNitsValue, 1.0f);
    const float x = qBound(0.0, (pixel.x() - plot.left()) / plot.width(), 1.0);
    const float y = qBound(0.0, (plot.bottom() - pixel.y()) / plot.height(), 1.0);
    return QPointF(x * inputSpan, y * outputSpan);
}

AutoHdr::ToneCurveEndpoints ToneCurveEditor::endpoints() const
{
    AutoHdr::ToneCurveEndpoints ep;
    ep.peakNits = m_peakNitsValue;
    ep.sdrMaxPoint = m_sdrMaxPoint;
    ep.visualReferenceNits = m_referenceNitsValue;
    return ep;
}

QVector<QPointF> ToneCurveEditor::fullCurve() const
{
    return AutoHdr::buildFullCurve(endpoints(), m_intermediatePoints);
}

int ToneCurveEditor::hitTest(const QPointF &pos) const
{
    const QPointF sdrPixel = nitsToPixel(static_cast<float>(m_sdrMaxPoint.x()), static_cast<float>(m_sdrMaxPoint.y()));
    if (QLineF(pos, sdrPixel).length() <= kHandleRadius + kHitPadding) {
        return -2;
    }

    for (int i = 0; i < m_intermediatePoints.size(); ++i) {
        const QPointF point = m_intermediatePoints.at(i);
        const QPointF pixel = nitsToPixel(static_cast<float>(point.x()), static_cast<float>(point.y()));
        if (QLineF(pos, pixel).length() <= kHandleRadius + kHitPadding) {
            return i;
        }
    }
    return -1;
}

void ToneCurveEditor::updateReadout(float inputNits)
{
    const float output = AutoHdr::evaluateToneCurve(fullCurve(), inputNits);
    m_inputLabel->setText(tr("Input: %1 nits").arg(qRound(inputNits)));
    m_outputLabel->setText(tr("Output: %1 nits").arg(qRound(output)));
}

int ToneCurveEditor::peakMin() const
{
    return qRound(m_referenceNitsValue) + 1;
}

void ToneCurveEditor::syncSpinRanges()
{
    m_peakNits->setRange(peakMin(), m_maxDisplayNits);
    m_referenceNits->setMaximum(qMin(static_cast<int>(AutoHdr::kReferenceNitsMax),
                                     qMax(static_cast<int>(AutoHdr::kReferenceNitsMin), qRound(m_peakNitsValue) - 1)));
}

void ToneCurveEditor::enforcePeakFloor()
{
    const int minPeak = peakMin();
    if (m_peakNitsValue < minPeak) {
        const float previousPeak = m_peakNitsValue;
        m_peakNitsValue = static_cast<float>(minPeak);
        m_peakNits->setValue(minPeak);
        maybeSnapSdrMaxToPeak(previousPeak);
    }
    syncSpinRanges();
}

void ToneCurveEditor::readSpinValuesFromUi()
{
    m_peakNits->interpretText();
    m_referenceNits->interpretText();
    m_peakNitsValue = static_cast<float>(m_peakNits->value());
    m_referenceNitsValue =
        AutoHdr::clampReferenceNits(static_cast<float>(m_referenceNits->value()));
}

void ToneCurveEditor::maybeSnapSdrMaxToPeak(float previousPeak)
{
    const float ref = m_referenceNitsValue;
    const float x = static_cast<float>(m_sdrMaxPoint.x());
    const float y = static_cast<float>(m_sdrMaxPoint.y());
    if (qAbs(x - ref) <= 1.0f && qAbs(y - previousPeak) <= 1.0f) {
        m_sdrMaxPoint = QPointF(ref, m_peakNitsValue);
    }
}

void ToneCurveEditor::flushSpinValuesFromUi()
{
    readSpinValuesFromUi();
    sanitizePoints();
}

void ToneCurveEditor::sanitizePoints()
{
    AutoHdr::ToneCurveEndpoints ep = endpoints();
    m_intermediatePoints = AutoHdr::sanitizeIntermediatePoints(m_intermediatePoints, ep);
    m_sdrMaxPoint = AutoHdr::sanitizeSdrMaxPoint(m_sdrMaxPoint, ep, m_intermediatePoints);
}

void ToneCurveEditor::emitChanged(bool committed)
{
    QObject *source = sender();
    if (source == m_referenceNits) {
        m_referenceNits->interpretText();
        m_peakNits->interpretText();
        m_peakNitsValue = static_cast<float>(m_peakNits->value());
        m_referenceNitsValue =
            AutoHdr::clampReferenceNits(static_cast<float>(m_referenceNits->value()));
        enforcePeakFloor();
    } else if (source == m_peakNits) {
        m_peakNits->interpretText();
        const float previousPeak = m_peakNitsValue;
        m_peakNitsValue = static_cast<float>(m_peakNits->value());
        if (qAbs(previousPeak - m_peakNitsValue) > 1e-3f) {
            maybeSnapSdrMaxToPeak(previousPeak);
        }
        syncSpinRanges();
    } else {
        readSpinValuesFromUi();
    }
    sanitizePoints();
    update();
    Q_EMIT settingsChanged();
    if (committed) {
        Q_EMIT settingsCommitted();
    }
}

void ToneCurveEditor::removeIntermediateAt(int index)
{
    if (index < 0 || index >= m_intermediatePoints.size()) {
        return;
    }
    m_intermediatePoints.removeAt(index);
    m_selectedIndex = -1;
    emitChanged(true);
}

void ToneCurveEditor::drawReferenceOverlay(QPainter &painter, const QRectF &plot) const
{
    const float reference = m_referenceNitsValue;
    if (reference <= 0.0f) {
        return;
    }

    const QPointF refCorner = nitsToPixel(reference, reference);
    const QRectF sdrRect(plot.left(), refCorner.y(), plot.width(), plot.bottom() - refCorner.y());
    painter.fillRect(sdrRect, QColor(70, 110, 160, 50));

    if (reference < m_peakNitsValue) {
        painter.setPen(QPen(QColor(90, 140, 200, 140), 1, Qt::DashLine));
        painter.drawLine(nitsToPixel(0.0f, reference), nitsToPixel(reference, reference));
    }
}

void ToneCurveEditor::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF plot = plotRect();

    painter.fillRect(plot, QColor(32, 32, 32));

    painter.setPen(QColor(70, 70, 70));
    for (int i = 1; i < 4; ++i) {
        const qreal t = plot.left() + (plot.width() * i) / 4.0;
        painter.drawLine(QPointF(t, plot.top()), QPointF(t, plot.bottom()));
        const qreal u = plot.top() + (plot.height() * i) / 4.0;
        painter.drawLine(QPointF(plot.left(), u), QPointF(plot.right(), u));
    }

    drawReferenceOverlay(painter, plot);

    painter.setPen(QColor(90, 90, 90));
    painter.drawLine(nitsToPixel(0.0f, 0.0f),
                     nitsToPixel(m_referenceNitsValue, m_referenceNitsValue));

    const QVector<QPointF> curve = fullCurve();
    if (curve.size() >= 2) {
        QPainterPath path;
        const int samples = 128;
        const float startX = static_cast<float>(curve.first().x());
        const float endX = static_cast<float>(curve.last().x());
        for (int i = 0; i <= samples; ++i) {
            const float input = startX + (endX - startX) * (static_cast<float>(i) / samples);
            const float output = AutoHdr::evaluateToneCurve(curve, input);
            const QPointF pixel = nitsToPixel(input, output);
            if (i == 0) {
                path.moveTo(pixel);
            } else {
                path.lineTo(pixel);
            }
        }
        painter.setPen(QPen(QColor(240, 240, 240), 2));
        painter.drawPath(path);
    }

    painter.setBrush(QColor(160, 160, 160));
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(nitsToPixel(0.0f, 0.0f), 4, 4);

    painter.setBrush(QColor(255, 255, 255));
    for (const QPointF &point : m_intermediatePoints) {
        painter.drawEllipse(nitsToPixel(static_cast<float>(point.x()), static_cast<float>(point.y())), kHandleRadius,
                            kHandleRadius);
    }

    painter.drawEllipse(nitsToPixel(static_cast<float>(m_sdrMaxPoint.x()), static_cast<float>(m_sdrMaxPoint.y())),
                        kHandleRadius, kHandleRadius);

    painter.setPen(QColor(180, 180, 180));
    painter.drawText(QRectF(plot.left(), plot.bottom() + 6, plot.width() / 2, 16), Qt::AlignLeft, tr("0 nits"));
    painter.drawText(QRectF(plot.center().x(), plot.bottom() + 6, plot.width() / 2, 16), Qt::AlignRight,
                     tr("%1 SDR nits").arg(qRound(m_referenceNitsValue)));
    painter.drawText(QRectF(plot.left() - 18, plot.top() - 4, 60, 16), Qt::AlignLeft,
                     tr("%1 nits").arg(qRound(m_peakNitsValue)));
}

void ToneCurveEditor::mousePressEvent(QMouseEvent *event)
{
    const int hit = hitTest(event->position());
    if ((event->modifiers() & Qt::ControlModifier) && hit >= 0) {
        removeIntermediateAt(hit);
        return;
    }
    if (hit == -2) {
        m_dragTarget = DragTarget::SdrMax;
        m_selectedIndex = -2;
        setFocus();
    } else if (hit >= 0) {
        m_dragTarget = DragTarget::Intermediate;
        m_dragIndex = hit;
        m_selectedIndex = hit;
        setFocus();
    } else if (!plotRect().contains(event->position())) {
        return;
    } else {
        return;
    }

    updateReadout(static_cast<float>(pixelToNits(event->position()).x()));
}

void ToneCurveEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragTarget == DragTarget::None) {
        if (interactiveRect().contains(event->position())) {
            updateReadout(static_cast<float>(pixelToNits(event->position()).x()));
        }
        return;
    }

    const QPointF nits = pixelToNits(event->position());

    const float maxInputNits = m_referenceNitsValue;

    if (m_dragTarget == DragTarget::SdrMax) {
        const double clampedX = qBound(0.0, nits.x(), static_cast<double>(maxInputNits));
        const double clampedY = qBound(0.0, nits.y(), static_cast<double>(m_peakNitsValue));
        if (qAbs(m_sdrMaxPoint.x() - maxInputNits) <= 1.0) {
            m_sdrMaxPoint = QPointF(maxInputNits, clampedY);
        } else {
            m_sdrMaxPoint = QPointF(clampedX, clampedY);
        }
        sanitizePoints();
    } else if (m_dragTarget == DragTarget::Intermediate && m_dragIndex >= 0
               && m_dragIndex < m_intermediatePoints.size()) {
        float x = qBound(1.0f, static_cast<float>(nits.x()), maxInputNits - 1.0f);
        float y = qBound(0.0f, static_cast<float>(nits.y()), m_peakNitsValue);

        const float prevX = m_dragIndex > 0 ? static_cast<float>(m_intermediatePoints[m_dragIndex - 1].x()) : 0.0f;
        const float nextX = m_dragIndex + 1 < m_intermediatePoints.size()
            ? static_cast<float>(m_intermediatePoints[m_dragIndex + 1].x())
            : static_cast<float>(m_sdrMaxPoint.x()) - 1.0f;
        x = qBound(prevX + 1.0f, x, nextX - 1.0f);

        const float prevY = m_dragIndex > 0 ? static_cast<float>(m_intermediatePoints[m_dragIndex - 1].y()) : 0.0f;
        y = qMax(prevY, y);

        m_intermediatePoints[m_dragIndex] = QPointF(x, y);
        sanitizePoints();
    }

    updateReadout(static_cast<float>(nits.x()));
    update();
    Q_EMIT settingsChanged();
}

void ToneCurveEditor::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event)
    if (m_dragTarget != DragTarget::None) {
        m_dragTarget = DragTarget::None;
        m_dragIndex = -1;
        emitChanged(true);
    }
}

void ToneCurveEditor::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!plotRect().contains(event->position()) || hitTest(event->position()) != -1) {
        return;
    }

    const QPointF nits = pixelToNits(event->position());
    float input = qBound(1.0f, static_cast<float>(nits.x()), static_cast<float>(m_sdrMaxPoint.x()) - 1.0f);
    float output = qBound(0.0f, static_cast<float>(nits.y()), m_peakNitsValue);
    m_intermediatePoints.append(QPointF(input, output));
    sanitizePoints();
    emitChanged(true);
}

void ToneCurveEditor::contextMenuEvent(QContextMenuEvent *event)
{
    const QPointF pos = QPointF(event->pos());
    const int hit = hitTest(pos);
    if (hit < 0) {
        return;
    }

    QMenu menu(this);
    QAction *removeAction = menu.addAction(tr("Remove point"));
    if (menu.exec(event->globalPos()) == removeAction) {
        removeIntermediateAt(hit);
    }
}

void ToneCurveEditor::keyPressEvent(QKeyEvent *event)
{
    if (event->key() != Qt::Key_Delete && event->key() != Qt::Key_Backspace) {
        QWidget::keyPressEvent(event);
        return;
    }

    if (m_selectedIndex < 0 || m_selectedIndex >= m_intermediatePoints.size()) {
        return;
    }

    removeIntermediateAt(m_selectedIndex);
}
