/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "autohdr_config.h"
#include "ui/tone_curve_editor.h"

#include <KSharedConfig>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QWidget>

class QDialogButtonBox;
class QPushButton;

class CalibrationOverlay : public QWidget
{
    Q_OBJECT

public:
    explicit CalibrationOverlay(QWidget *parent = nullptr);

    void setConfig(const KSharedConfigPtr &config);
    void setHdrLimits(int minPeakNits, int maxDisplayNits);
    void setValues(const AutoHdr::CalibrationSettings &settings);
    void setPerceptualColorEnabled(bool enabled);
    bool perceptualColorEnabled() const;
    void setGlobalAiEnabled(bool enabled);
    AutoHdr::CalibrationSettings currentValues() const;
    QRect panelBlurRegion() const;

Q_SIGNALS:
    void settingsChanged();
    void settingsCommitted();
    void confirmed();
    void cancelled();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void onConfirm();
    void onCancel();

    void repositionPanel();

    QWidget *m_contentPanel = nullptr;
    ToneCurveEditor *m_editor = nullptr;
    QCheckBox *m_perceptualColor = nullptr;
    QCheckBox *m_aiEnhanced = nullptr;
    QDoubleSpinBox *m_aiStrength = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};
