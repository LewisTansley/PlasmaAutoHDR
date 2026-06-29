/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "tone_curve_editor.h"

#include "tone_curve.h"
#include "tone_curve_presets.h"
#include "tone_curve_user_presets.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

constexpr int kPlotSize = 280;
constexpr int kPlotLabelTop = 18;
constexpr int kPlotLabelBottom = 20;

} // namespace

ToneCurveEditor::ToneCurveEditor(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(kPlotSize + 180, kPlotSize + 56);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto *presetLayout = new QHBoxLayout();
    m_presetCombo = new QComboBox(this);
    m_savePresetBtn = new QPushButton(tr("Save preset…"), this);
    m_updatePresetBtn = new QPushButton(tr("Update preset"), this);
    m_deletePresetBtn = new QPushButton(tr("Delete preset"), this);
    presetLayout->addWidget(new QLabel(tr("Curve preset:"), this));
    presetLayout->addWidget(m_presetCombo, 1);
    presetLayout->addWidget(m_savePresetBtn);
    presetLayout->addWidget(m_updatePresetBtn);
    presetLayout->addWidget(m_deletePresetBtn);
    layout->addLayout(presetLayout);

    auto *bodyLayout = new QHBoxLayout();
    bodyLayout->setSpacing(16);
    m_plotHost = new QWidget(this);
    m_plotHost->setMinimumSize(kPlotSize, kPlotSize);
    m_plotHost->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_plotHost->installEventFilter(this);
    bodyLayout->addWidget(m_plotHost);

    auto *controlsLayout = new QFormLayout();
    controlsLayout->setContentsMargins(0, 8, 0, 0);
    controlsLayout->setVerticalSpacing(8);
    m_peakNits = new QSpinBox(this);
    m_peakNits->setRange(1, 10000);
    m_peakNits->setSingleStep(10);
    m_referenceNits = new QSpinBox(this);
    m_referenceNits->setRange(static_cast<int>(AutoHdr::kReferenceNitsMin),
                              static_cast<int>(AutoHdr::kReferenceNitsMax));
    m_referenceNits->setSingleStep(1);
    m_blackPoint = new QDoubleSpinBox(this);
    m_blackPoint->setRange(-0.01, 0.01);
    m_blackPoint->setDecimals(4);
    m_blackPoint->setSingleStep(0.0001);
    m_blackPoint->setToolTip(tr("Fine-tune shadow lift on top of automatic 8-bit shadow roll-off. "
                                 "Leave at 0 for most content."));
    m_vibrance = new QDoubleSpinBox(this);
    m_vibrance->setRange(0.0, 10.0);
    m_vibrance->setSingleStep(0.1);
    m_gamutExpansion = new QDoubleSpinBox(this);
    m_gamutExpansion->setRange(0.0, 20.0);
    m_gamutExpansion->setSingleStep(0.1);
    m_gamutExpansion->setToolTip(tr("Boosts automatic saturation-aware gamut expansion. "
                                    "0 applies a reduced baseline; 1.5 matches the previous default strength."));
    m_chromaCompensation = new QDoubleSpinBox(this);
    m_chromaCompensation->setRange(0.0, 1.0);
    m_chromaCompensation->setSingleStep(0.05);
    m_chromaCompensation->setToolTip(tr("Compensates for chroma loss when luminance is expanded in ICtCp space. "
                                        "Leave at 0 unless highlights look desaturated."));
    m_highlightRolloff = new QDoubleSpinBox(this);
    m_highlightRolloff->setRange(0.0, 1.0);
    m_highlightRolloff->setSingleStep(0.05);
    m_highlightRolloff->setToolTip(tr("Smooth ICtCp highlight compression above 85% of display peak. "
                                      "0 keeps the legacy hard peak clip."));
    m_gamutMappingStrength = new QDoubleSpinBox(this);
    m_gamutMappingStrength->setRange(0.0, 1.0);
    m_gamutMappingStrength->setSingleStep(0.05);
    m_gamutMappingStrength->setToolTip(tr("Tames out-of-gamut colors after gamut expansion. "
                                          "0 disables post-expansion gamut mapping."));
    m_inputLabel = new QLabel(tr("Input: —"), this);
    m_outputLabel = new QLabel(tr("Output: —"), this);
    controlsLayout->addRow(tr("Peak nits:"), m_peakNits);
    controlsLayout->addRow(tr("Reference nits:"), m_referenceNits);
    controlsLayout->addRow(tr("Shadow offset:"), m_blackPoint);
    controlsLayout->addRow(tr("Vibrance:"), m_vibrance);
    controlsLayout->addRow(tr("Gamut expansion boost:"), m_gamutExpansion);
    controlsLayout->addRow(tr("Chroma compensation:"), m_chromaCompensation);
    controlsLayout->addRow(tr("Highlight rolloff:"), m_highlightRolloff);
    controlsLayout->addRow(tr("Gamut mapping:"), m_gamutMappingStrength);
    controlsLayout->addRow(m_inputLabel);
    controlsLayout->addRow(m_outputLabel);
    bodyLayout->addLayout(controlsLayout, 1);
    layout->addLayout(bodyLayout);

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
        applyActivePreset();
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
        applyActivePreset();
        sanitizePoints();
        update();
        Q_EMIT settingsChanged();
    });
    connect(m_referenceNits, &QSpinBox::editingFinished, this, [this]() { emitChanged(true); });
    connect(m_blackPoint, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this]() {
        if (m_blockSignals) {
            return;
        }
        m_blackPointValue = AutoHdr::clampBlackPoint(static_cast<float>(m_blackPoint->value()));
        Q_EMIT settingsChanged();
    });
    connect(m_blackPoint, &QDoubleSpinBox::editingFinished, this, [this]() { emitChanged(true); });
    connect(m_vibrance, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this]() {
        if (m_blockSignals) {
            return;
        }
        m_vibranceValue = AutoHdr::clampVibrance(static_cast<float>(m_vibrance->value()));
        Q_EMIT settingsChanged();
    });
    connect(m_vibrance, &QDoubleSpinBox::editingFinished, this, [this]() { emitChanged(true); });
    connect(m_gamutExpansion, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this]() {
        if (m_blockSignals) {
            return;
        }
        m_gamutExpansionValue = AutoHdr::clampGamutExpansion(static_cast<float>(m_gamutExpansion->value()));
        Q_EMIT settingsChanged();
    });
    connect(m_gamutExpansion, &QDoubleSpinBox::editingFinished, this, [this]() { emitChanged(true); });
    connect(m_chromaCompensation, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this]() {
        if (m_blockSignals) {
            return;
        }
        m_chromaCompensationValue =
            AutoHdr::clampChromaCompensation(static_cast<float>(m_chromaCompensation->value()));
        Q_EMIT settingsChanged();
    });
    connect(m_chromaCompensation, &QDoubleSpinBox::editingFinished, this, [this]() { emitChanged(true); });
    connect(m_highlightRolloff, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this]() {
        if (m_blockSignals) {
            return;
        }
        m_highlightRolloffValue = AutoHdr::clampHighlightRolloff(static_cast<float>(m_highlightRolloff->value()));
        Q_EMIT settingsChanged();
    });
    connect(m_highlightRolloff, &QDoubleSpinBox::editingFinished, this, [this]() { emitChanged(true); });
    connect(m_gamutMappingStrength, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this]() {
        if (m_blockSignals) {
            return;
        }
        m_gamutMappingStrengthValue =
            AutoHdr::clampGamutMappingStrength(static_cast<float>(m_gamutMappingStrength->value()));
        Q_EMIT settingsChanged();
    });
    connect(m_gamutMappingStrength, &QDoubleSpinBox::editingFinished, this, [this]() { emitChanged(true); });

    connect(m_presetCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_blockSignals || index < 0) {
            return;
        }
        const QString comboId = m_presetCombo->itemData(index).toString();
        if (comboId == QStringLiteral("custom")) {
            return;
        }
        setPresetFromComboId(comboId, true);
        update();
        Q_EMIT settingsChanged();
    });
    connect(m_presetCombo, &QComboBox::activated, this, [this]() { emitChanged(true); });
    connect(m_savePresetBtn, &QPushButton::clicked, this, [this]() { saveUserPreset(); });
    connect(m_updatePresetBtn, &QPushButton::clicked, this, [this]() { updateUserPreset(); });
    connect(m_deletePresetBtn, &QPushButton::clicked, this, [this]() { deleteUserPreset(); });

    rebuildPresetCombo();
    updatePresetActionButtons();
}

void ToneCurveEditor::setConfig(const KSharedConfigPtr &config)
{
    m_config = config;
    rebuildPresetCombo();
    syncPresetCombo();
    updatePresetActionButtons();
}

void ToneCurveEditor::setHdrLimits(int minPeakNits, int maxDisplayNits)
{
    m_minPeakNits = minPeakNits;
    m_maxDisplayNits = maxDisplayNits;
    syncSpinRanges();
}

void ToneCurveEditor::setValues(float peakNits, float referenceNits, const QPointF &sdrMaxPoint,
                                const QVector<QPointF> &intermediatePoints, float blackPoint,
                                AutoHdr::ToneCurvePreset preset, const QString &userPresetId, float vibrance,
                                float gamutExpansion, float chromaCompensation, float highlightRolloff,
                                float gamutMappingStrength)
{
    m_blockSignals = true;
    m_peakNitsValue = peakNits;
    m_referenceNitsValue = referenceNits;
    m_sdrMaxPoint = sdrMaxPoint;
    m_intermediatePoints = intermediatePoints;
    m_blackPointValue = AutoHdr::clampBlackPoint(blackPoint);
    m_vibranceValue = AutoHdr::clampVibrance(vibrance);
    m_gamutExpansionValue = AutoHdr::clampGamutExpansion(gamutExpansion);
    m_chromaCompensationValue = AutoHdr::clampChromaCompensation(chromaCompensation);
    m_highlightRolloffValue = AutoHdr::clampHighlightRolloff(highlightRolloff);
    m_gamutMappingStrengthValue = AutoHdr::clampGamutMappingStrength(gamutMappingStrength);
    m_preset = preset;
    m_userPresetId = userPresetId;
    syncSpinRanges();
    m_peakNits->setValue(qRound(peakNits));
    m_referenceNits->setValue(qRound(referenceNits));
    m_blackPoint->setValue(m_blackPointValue);
    m_vibrance->setValue(m_vibranceValue);
    m_gamutExpansion->setValue(m_gamutExpansionValue);
    m_chromaCompensation->setValue(m_chromaCompensationValue);
    m_highlightRolloff->setValue(m_highlightRolloffValue);
    m_gamutMappingStrength->setValue(m_gamutMappingStrengthValue);
    enforcePeakFloor();
    sanitizePoints();
    rebuildPresetCombo();
    syncPresetCombo();
    updatePresetActionButtons();
    m_blockSignals = false;
    update();
}

void ToneCurveEditor::getValues(float &peakNits, float &referenceNits, QPointF &sdrMaxPoint,
                                QVector<QPointF> &intermediatePoints, float &blackPoint,
                                AutoHdr::ToneCurvePreset &preset, QString &userPresetId, float &vibrance,
                                float &gamutExpansion, float &chromaCompensation, float &highlightRolloff,
                                float &gamutMappingStrength)
{
    flushSpinValuesFromUi();
    peakNits = m_peakNitsValue;
    referenceNits = m_referenceNitsValue;
    sdrMaxPoint = m_sdrMaxPoint;
    intermediatePoints = m_intermediatePoints;
    m_blackPoint->interpretText();
    m_blackPointValue = AutoHdr::clampBlackPoint(static_cast<float>(m_blackPoint->value()));
    blackPoint = m_blackPointValue;
    m_vibrance->interpretText();
    m_vibranceValue = AutoHdr::clampVibrance(static_cast<float>(m_vibrance->value()));
    vibrance = m_vibranceValue;
    m_gamutExpansion->interpretText();
    m_gamutExpansionValue = AutoHdr::clampGamutExpansion(static_cast<float>(m_gamutExpansion->value()));
    gamutExpansion = m_gamutExpansionValue;
    m_chromaCompensation->interpretText();
    m_chromaCompensationValue =
        AutoHdr::clampChromaCompensation(static_cast<float>(m_chromaCompensation->value()));
    chromaCompensation = m_chromaCompensationValue;
    m_highlightRolloff->interpretText();
    m_highlightRolloffValue = AutoHdr::clampHighlightRolloff(static_cast<float>(m_highlightRolloff->value()));
    highlightRolloff = m_highlightRolloffValue;
    m_gamutMappingStrength->interpretText();
    m_gamutMappingStrengthValue =
        AutoHdr::clampGamutMappingStrength(static_cast<float>(m_gamutMappingStrength->value()));
    gamutMappingStrength = m_gamutMappingStrengthValue;
    preset = m_preset;
    userPresetId = m_userPresetId;
}

AutoHdr::ToneCurvePreset ToneCurveEditor::toneCurvePreset() const
{
    return m_preset;
}

float ToneCurveEditor::blackPoint() const
{
    return m_blackPointValue;
}

void ToneCurveEditor::setBlackPoint(float blackPoint)
{
    m_blockSignals = true;
    m_blackPointValue = AutoHdr::clampBlackPoint(blackPoint);
    m_blackPoint->setValue(m_blackPointValue);
    m_blockSignals = false;
}

QRectF ToneCurveEditor::plotHostRect() const
{
    if (!m_plotHost) {
        return QRectF(0.0, 0.0, kPlotSize, kPlotSize);
    }
    const QPoint topLeft = m_plotHost->mapTo(this, QPoint(0, 0));
    return QRectF(topLeft.x(), topLeft.y(), m_plotHost->width(), m_plotHost->height());
}

QRectF ToneCurveEditor::plotRect() const
{
    return plotHostRect().adjusted(0, kPlotLabelTop, 0, -kPlotLabelBottom);
}

QRectF ToneCurveEditor::interactiveRect() const
{
    return plotRect().adjusted(-kHitPadding, -kHitPadding, kHitPadding, kHitPadding);
}

QPointF ToneCurveEditor::mapPlotEventPos(const QPointF &plotLocalPos) const
{
    const QRectF plot = plotRect();
    return QPointF(plot.left() + plotLocalPos.x(), plot.top() + plotLocalPos.y() - kPlotLabelTop);
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

AutoHdr::CalibrationSettings ToneCurveEditor::currentCalibrationSettings() const
{
    AutoHdr::CalibrationSettings settings;
    settings.maxNits = m_peakNitsValue;
    settings.referenceNits = m_referenceNitsValue;
    settings.sdrMaxPoint = m_sdrMaxPoint;
    settings.toneCurvePoints = m_intermediatePoints;
    settings.toneCurvePreset = m_preset;
    settings.toneCurveUserPresetId = m_userPresetId;
    return settings;
}

void ToneCurveEditor::applyActivePreset()
{
    if (m_preset == AutoHdr::ToneCurvePreset::Custom) {
        return;
    }

    AutoHdr::CalibrationSettings settings = currentCalibrationSettings();
    AutoHdr::applyToneCurvePreset(settings, m_config);
    m_sdrMaxPoint = settings.sdrMaxPoint;
    m_intermediatePoints = settings.toneCurvePoints;
}

void ToneCurveEditor::markCustomPreset()
{
    if (m_preset == AutoHdr::ToneCurvePreset::Custom) {
        return;
    }
    m_preset = AutoHdr::ToneCurvePreset::Custom;
    m_userPresetId.clear();
    syncPresetCombo();
    updatePresetActionButtons();
}

QString ToneCurveEditor::currentComboId() const
{
    if (m_preset == AutoHdr::ToneCurvePreset::Custom) {
        return QStringLiteral("custom");
    }
    if (m_preset == AutoHdr::ToneCurvePreset::User) {
        return AutoHdr::userPresetComboId(m_userPresetId);
    }
    return AutoHdr::presetToString(m_preset);
}

void ToneCurveEditor::rebuildPresetCombo()
{
    if (!m_presetCombo) {
        return;
    }

    m_blockSignals = true;
    const QString previousId = currentComboId();
    m_presetCombo->clear();

    for (AutoHdr::ToneCurvePreset preset : AutoHdr::builtInToneCurvePresets()) {
        m_presetCombo->addItem(AutoHdr::presetDisplayName(preset), AutoHdr::presetToString(preset));
    }

    if (m_config) {
        const QVector<AutoHdr::UserToneCurvePreset> userPresets = AutoHdr::loadUserToneCurvePresets(m_config);
        if (!userPresets.isEmpty()) {
            m_presetCombo->insertSeparator(m_presetCombo->count());
            for (const AutoHdr::UserToneCurvePreset &preset : userPresets) {
                m_presetCombo->addItem(preset.displayName, AutoHdr::userPresetComboId(preset.id));
            }
        }
    }

    m_presetCombo->insertSeparator(m_presetCombo->count());
    m_presetCombo->addItem(AutoHdr::presetDisplayName(AutoHdr::ToneCurvePreset::Custom), QStringLiteral("custom"));

    const int index = m_presetCombo->findData(previousId);
    if (index >= 0) {
        m_presetCombo->setCurrentIndex(index);
    }
    m_blockSignals = false;
}

void ToneCurveEditor::syncPresetCombo()
{
    if (!m_presetCombo) {
        return;
    }
    m_blockSignals = true;
    const int index = m_presetCombo->findData(currentComboId());
    if (index >= 0) {
        m_presetCombo->setCurrentIndex(index);
    }
    m_blockSignals = false;
}

void ToneCurveEditor::setPresetFromComboId(const QString &comboId, bool applyCurve)
{
    if (comboId == QStringLiteral("custom")) {
        return;
    }

    if (AutoHdr::isUserPresetComboId(comboId)) {
        m_preset = AutoHdr::ToneCurvePreset::User;
        m_userPresetId = AutoHdr::userPresetIdFromComboId(comboId);
    } else {
        m_preset = AutoHdr::presetFromString(comboId);
        m_userPresetId.clear();
    }

    syncPresetCombo();
    updatePresetActionButtons();
    if (applyCurve) {
        applyActivePreset();
        sanitizePoints();
    }
}

void ToneCurveEditor::updatePresetActionButtons()
{
    const bool hasConfig = static_cast<bool>(m_config);
    const bool userActive = m_preset == AutoHdr::ToneCurvePreset::User && !m_userPresetId.isEmpty();
    m_savePresetBtn->setEnabled(hasConfig);
    m_updatePresetBtn->setEnabled(hasConfig && userActive);
    m_deletePresetBtn->setEnabled(hasConfig && userActive);
}

void ToneCurveEditor::saveUserPreset()
{
    if (!m_config) {
        return;
    }

    flushSpinValuesFromUi();
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("Save Tone Curve Preset"), tr("Preset name:"), QLineEdit::Normal, QString(), &ok)
            .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }

    AutoHdr::UserToneCurvePreset preset = AutoHdr::normalizeCurrentCurve(currentCalibrationSettings());
    preset.id = AutoHdr::sanitizeUserPresetKey(name);
    preset.displayName = name;
    if (!AutoHdr::saveUserToneCurvePreset(m_config, preset)) {
        return;
    }

    m_preset = AutoHdr::ToneCurvePreset::User;
    m_userPresetId = preset.id;
    rebuildPresetCombo();
    syncPresetCombo();
    updatePresetActionButtons();
    update();
    Q_EMIT settingsChanged();
    Q_EMIT settingsCommitted();
}

void ToneCurveEditor::updateUserPreset()
{
    if (!m_config || m_preset != AutoHdr::ToneCurvePreset::User || m_userPresetId.isEmpty()) {
        return;
    }

    flushSpinValuesFromUi();
    AutoHdr::UserToneCurvePreset preset = AutoHdr::normalizeCurrentCurve(currentCalibrationSettings());
    preset.id = m_userPresetId;
    const std::optional<AutoHdr::UserToneCurvePreset> existing =
        AutoHdr::loadUserToneCurvePreset(m_config, m_userPresetId);
    preset.displayName = existing ? existing->displayName : m_userPresetId;
    AutoHdr::saveUserToneCurvePreset(m_config, preset);
    rebuildPresetCombo();
    syncPresetCombo();
    update();
    Q_EMIT settingsChanged();
    Q_EMIT settingsCommitted();
}

void ToneCurveEditor::deleteUserPreset()
{
    if (!m_config || m_preset != AutoHdr::ToneCurvePreset::User || m_userPresetId.isEmpty()) {
        return;
    }

    const auto answer = QMessageBox::question(this, tr("Delete Tone Curve Preset"),
                                              tr("Delete preset \"%1\"?").arg(m_userPresetId));
    if (answer != QMessageBox::Yes) {
        return;
    }

    AutoHdr::deleteUserToneCurvePreset(m_config, m_userPresetId);
    m_preset = AutoHdr::ToneCurvePreset::Custom;
    m_userPresetId.clear();
    rebuildPresetCombo();
    syncPresetCombo();
    updatePresetActionButtons();
    update();
    Q_EMIT settingsChanged();
    Q_EMIT settingsCommitted();
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
        applyActivePreset();
    } else if (source == m_peakNits) {
        m_peakNits->interpretText();
        const float previousPeak = m_peakNitsValue;
        m_peakNitsValue = static_cast<float>(m_peakNits->value());
        if (qAbs(previousPeak - m_peakNitsValue) > 1e-3f) {
            maybeSnapSdrMaxToPeak(previousPeak);
        }
        syncSpinRanges();
        applyActivePreset();
    } else if (source == m_blackPoint) {
        m_blackPoint->interpretText();
        m_blackPointValue = AutoHdr::clampBlackPoint(static_cast<float>(m_blackPoint->value()));
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
    markCustomPreset();
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

void ToneCurveEditor::drawPlot(QPainter &painter, const QRectF &plot) const
{
    const QRectF host = plotHostRect();
    painter.fillRect(host, QColor(32, 32, 32));

    painter.setPen(QColor(70, 70, 70));
    for (int i = 1; i < 4; ++i) {
        const qreal t = plot.left() + (plot.width() * i) / 4.0;
        painter.drawLine(QPointF(t, plot.top()), QPointF(t, plot.bottom()));
        const qreal u = plot.top() + (plot.height() * i) / 4.0;
        painter.drawLine(QPointF(plot.left(), u), QPointF(plot.right(), u));
    }

    drawReferenceOverlay(painter, plot);

    painter.setPen(QColor(90, 90, 90));
    painter.drawLine(nitsToPixel(0.0f, 0.0f), nitsToPixel(m_referenceNitsValue, m_referenceNitsValue));

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
    const qreal bottomLabelHeight = host.bottom() - plot.bottom();
    painter.drawText(QRectF(plot.left(), plot.bottom(), plot.width() / 2.0, bottomLabelHeight), Qt::AlignLeft | Qt::AlignTop,
                     tr("0 nits"));
    painter.drawText(QRectF(plot.center().x(), plot.bottom(), plot.width() / 2.0, bottomLabelHeight),
                     Qt::AlignRight | Qt::AlignTop, tr("%1 SDR nits").arg(qRound(m_referenceNitsValue)));
    painter.drawText(QRectF(host.left(), host.top(), host.width() * 0.55, kPlotLabelTop), Qt::AlignLeft | Qt::AlignVCenter,
                     tr("%1 nits").arg(qRound(m_peakNitsValue)));
}

void ToneCurveEditor::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawPlot(painter, plotRect());
}

bool ToneCurveEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_plotHost) {
        return QWidget::eventFilter(watched, event);
    }

    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
    case QEvent::MouseButtonDblClick:
    case QEvent::ContextMenu: {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        const QPointF mappedPos = mapPlotEventPos(mouseEvent->position());

        switch (event->type()) {
        case QEvent::MouseButtonPress:
            handlePlotMousePress(mappedPos, mouseEvent->modifiers());
            return true;
        case QEvent::MouseMove:
            handlePlotMouseMove(mappedPos);
            return true;
        case QEvent::MouseButtonRelease:
            handlePlotMouseRelease();
            return true;
        case QEvent::MouseButtonDblClick:
            handlePlotMouseDoubleClick(mappedPos);
            return true;
        case QEvent::ContextMenu:
            handlePlotContextMenu(mappedPos, mouseEvent->globalPosition().toPoint());
            return true;
        default:
            break;
        }
        break;
    }
    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}

void ToneCurveEditor::handlePlotMousePress(const QPointF &pos, Qt::KeyboardModifiers modifiers)
{
    const int hit = hitTest(pos);
    if ((modifiers & Qt::ControlModifier) && hit >= 0) {
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
    } else if (!plotRect().contains(pos)) {
        return;
    } else {
        return;
    }

    updateReadout(static_cast<float>(pixelToNits(pos).x()));
}

void ToneCurveEditor::handlePlotMouseMove(const QPointF &pos)
{
    if (m_dragTarget == DragTarget::None) {
        if (interactiveRect().contains(pos)) {
            updateReadout(static_cast<float>(pixelToNits(pos).x()));
        }
        return;
    }

    markCustomPreset();

    const QPointF nits = pixelToNits(pos);
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

void ToneCurveEditor::handlePlotMouseRelease()
{
    if (m_dragTarget != DragTarget::None) {
        m_dragTarget = DragTarget::None;
        m_dragIndex = -1;
        emitChanged(true);
    }
}

void ToneCurveEditor::handlePlotMouseDoubleClick(const QPointF &pos)
{
    if (!plotRect().contains(pos) || hitTest(pos) != -1) {
        return;
    }

    const QPointF nits = pixelToNits(pos);
    float input = qBound(1.0f, static_cast<float>(nits.x()), static_cast<float>(m_sdrMaxPoint.x()) - 1.0f);
    float output = qBound(0.0f, static_cast<float>(nits.y()), m_peakNitsValue);
    markCustomPreset();
    m_intermediatePoints.append(QPointF(input, output));
    sanitizePoints();
    emitChanged(true);
}

void ToneCurveEditor::handlePlotContextMenu(const QPointF &pos, const QPoint &globalPos)
{
    const int hit = hitTest(pos);
    if (hit < 0) {
        return;
    }

    QMenu menu(this);
    QAction *removeAction = menu.addAction(tr("Remove point"));
    if (menu.exec(globalPos) == removeAction) {
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
