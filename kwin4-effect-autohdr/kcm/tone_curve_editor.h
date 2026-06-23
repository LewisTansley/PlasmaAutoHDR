/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "autohdr_config.h"

#include <QWidget>

class QLabel;
class QSpinBox;

class ToneCurveEditor : public QWidget
{
    Q_OBJECT

public:
    explicit ToneCurveEditor(QWidget *parent = nullptr);

    void setHdrLimits(int minPeakNits, int maxDisplayNits);
    void setValues(float peakNits, float referenceNits, const QPointF &sdrMaxPoint,
                   const QVector<QPointF> &intermediatePoints);
    void getValues(float &peakNits, float &referenceNits, QPointF &sdrMaxPoint,
                   QVector<QPointF> &intermediatePoints);

    void seedFromLegacy(float midPointNits, float peakNits);

Q_SIGNALS:
    void settingsChanged();
    void settingsCommitted();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    enum class DragTarget {
        None,
        SdrMax,
        Intermediate,
    };

    static constexpr int kHandleRadius = 6;
    static constexpr int kHitPadding = 8;

    QRectF plotRect() const;
    QRectF interactiveRect() const;
    QPointF nitsToPixel(float inputNits, float outputNits) const;
    QPointF pixelToNits(const QPointF &pixel) const;
    AutoHdr::ToneCurveEndpoints endpoints() const;
    QVector<QPointF> fullCurve() const;
    int hitTest(const QPointF &pos) const;
    void updateReadout(float inputNits);
    int peakMin() const;
    void syncSpinRanges();
    void enforcePeakFloor();
    void readSpinValuesFromUi();
    void maybeSnapSdrMaxToPeak(float previousPeak);
    void flushSpinValuesFromUi();
    void sanitizePoints();
    void emitChanged(bool committed);
    void removeIntermediateAt(int index);
    void drawReferenceOverlay(QPainter &painter, const QRectF &plot) const;

    QSpinBox *m_peakNits = nullptr;
    QSpinBox *m_referenceNits = nullptr;
    QLabel *m_inputLabel = nullptr;
    QLabel *m_outputLabel = nullptr;

    int m_minPeakNits = 101;
    int m_maxDisplayNits = 1000;
    float m_peakNitsValue = 1000.0f;
    float m_referenceNitsValue = 203.0f;
    QPointF m_sdrMaxPoint;
    QVector<QPointF> m_intermediatePoints;

    DragTarget m_dragTarget = DragTarget::None;
    int m_dragIndex = -1;
    int m_selectedIndex = -1;
    bool m_blockSignals = false;
};
