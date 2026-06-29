/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "autohdr_config.h"

#include <KSharedConfig>
#include <QWidget>

class QComboBox;
class QLabel;
class QDoubleSpinBox;
class QPushButton;
class QSpinBox;

class ToneCurveEditor : public QWidget
{
    Q_OBJECT

public:
    explicit ToneCurveEditor(QWidget *parent = nullptr);

    void setConfig(const KSharedConfigPtr &config);
    void setHdrLimits(int minPeakNits, int maxDisplayNits);
    void setValues(float peakNits, float referenceNits, const QPointF &sdrMaxPoint,
                   const QVector<QPointF> &intermediatePoints, float blackPoint = 0.0f,
                   AutoHdr::ToneCurvePreset preset = AutoHdr::ToneCurvePreset::Linear,
                   const QString &userPresetId = QString(), float vibrance = 0.0f,
                   float gamutExpansion = 1.5f, float chromaCompensation = 0.0f,
                   float highlightRolloff = 0.0f);
    void getValues(float &peakNits, float &referenceNits, QPointF &sdrMaxPoint,
                   QVector<QPointF> &intermediatePoints, float &blackPoint, AutoHdr::ToneCurvePreset &preset,
                   QString &userPresetId, float &vibrance, float &gamutExpansion, float &chromaCompensation,
                   float &highlightRolloff);

    AutoHdr::ToneCurvePreset toneCurvePreset() const;

    float blackPoint() const;
    void setBlackPoint(float blackPoint);

Q_SIGNALS:
    void settingsChanged();
    void settingsCommitted();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    enum class DragTarget {
        None,
        SdrMax,
        Intermediate,
    };

    static constexpr int kHandleRadius = 6;
    static constexpr int kHitPadding = 8;

    QRectF plotHostRect() const;
    QRectF plotRect() const;
    QRectF interactiveRect() const;
    QPointF mapPlotEventPos(const QPointF &plotLocalPos) const;
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
    void drawPlot(QPainter &painter, const QRectF &plot) const;
    void applyActivePreset();
    void markCustomPreset();
    void rebuildPresetCombo();
    void syncPresetCombo();
    void setPresetFromComboId(const QString &comboId, bool applyCurve);
    QString currentComboId() const;
    void updatePresetActionButtons();
    AutoHdr::CalibrationSettings currentCalibrationSettings() const;
    void saveUserPreset();
    void updateUserPreset();
    void deleteUserPreset();
    void handlePlotMousePress(const QPointF &plotPos, Qt::KeyboardModifiers modifiers);
    void handlePlotMouseMove(const QPointF &plotPos);
    void handlePlotMouseRelease();
    void handlePlotMouseDoubleClick(const QPointF &plotPos);
    void handlePlotContextMenu(const QPointF &plotPos, const QPoint &globalPos);

    KSharedConfigPtr m_config;
    QComboBox *m_presetCombo = nullptr;
    QPushButton *m_savePresetBtn = nullptr;
    QPushButton *m_updatePresetBtn = nullptr;
    QPushButton *m_deletePresetBtn = nullptr;
    QWidget *m_plotHost = nullptr;
    QSpinBox *m_peakNits = nullptr;
    QSpinBox *m_referenceNits = nullptr;
    QDoubleSpinBox *m_blackPoint = nullptr;
    QDoubleSpinBox *m_vibrance = nullptr;
    QDoubleSpinBox *m_gamutExpansion = nullptr;
    QDoubleSpinBox *m_chromaCompensation = nullptr;
    QDoubleSpinBox *m_highlightRolloff = nullptr;
    QLabel *m_inputLabel = nullptr;
    QLabel *m_outputLabel = nullptr;

    int m_minPeakNits = 101;
    int m_maxDisplayNits = 1000;
    float m_peakNitsValue = 1000.0f;
    float m_referenceNitsValue = 203.0f;
    float m_blackPointValue = 0.0f;
    float m_vibranceValue = 0.0f;
    float m_gamutExpansionValue = 1.5f;
    float m_chromaCompensationValue = 0.0f;
    float m_highlightRolloffValue = 0.0f;
    QPointF m_sdrMaxPoint;
    QVector<QPointF> m_intermediatePoints;
    AutoHdr::ToneCurvePreset m_preset = AutoHdr::ToneCurvePreset::Linear;
    QString m_userPresetId;

    DragTarget m_dragTarget = DragTarget::None;
    int m_dragIndex = -1;
    int m_selectedIndex = -1;
    bool m_blockSignals = false;
};
