/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "calibration_overlay.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace {

constexpr int kPanelCornerRadius = 8;
constexpr int kPanelMargin = 12;

class OverlayPanel : public QWidget
{
public:
    explicit OverlayPanel(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event)

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        QPainterPath fillPath;
        fillPath.addRoundedRect(rect(), kPanelCornerRadius, kPanelCornerRadius);
        painter.fillPath(fillPath, QColor(18, 18, 18, 150));
    }
};

} // namespace

CalibrationOverlay::CalibrationOverlay(QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::StrongFocus);

    m_contentPanel = new OverlayPanel(this);

    auto *panelLayout = new QVBoxLayout(m_contentPanel);
    panelLayout->setContentsMargins(kPanelMargin, kPanelMargin, kPanelMargin, kPanelMargin);
    panelLayout->setSpacing(8);

    m_editor = new ToneCurveEditor(m_contentPanel);
    m_editor->setOverlayMode(true);
    m_editor->setOverlayLuminanceFactor(0.8f);
    panelLayout->addWidget(m_editor);

    m_perceptualColor = new QCheckBox(tr("Perceptual color (luminance/chroma separation)"), m_contentPanel);
    m_perceptualColor->setToolTip(
        tr("Off uses legacy RGB-coupled tone mapping. On applies PQ perceptual remapping in XYZ; "
           "color intensity blends Y-only vs full-XYZ PQ boost."));
    panelLayout->addWidget(m_perceptualColor);

    m_aiEnhanced = new QCheckBox(tr("AI-enhanced HDR (detail recovery)"), m_contentPanel);
    m_aiEnhanced->setToolTip(
        tr("Recovers shadow and highlight micro-detail lost to 8-bit SDR, adds perceptual "
           "depth in flat midtones, and expands lights, sky, and speculars."));
    panelLayout->addWidget(m_aiEnhanced);

    auto *aiStrengthRow = new QHBoxLayout();
    aiStrengthRow->addWidget(new QLabel(tr("AI strength:"), m_contentPanel));
    m_aiStrength = new QDoubleSpinBox(m_contentPanel);
    m_aiStrength->setRange(0.0, 100.0);
    m_aiStrength->setSuffix(QStringLiteral(" %"));
    m_aiStrength->setDecimals(0);
    m_aiStrength->setSingleStep(5.0);
    aiStrengthRow->addWidget(m_aiStrength);
    panelLayout->addLayout(aiStrengthRow);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, m_contentPanel);
    if (QPushButton *confirm = m_buttons->button(QDialogButtonBox::Ok)) {
        confirm->setText(tr("Confirm"));
    }
    panelLayout->addWidget(m_buttons);

    connect(m_editor, &ToneCurveEditor::settingsChanged, this, &CalibrationOverlay::settingsChanged);
    connect(m_editor, &ToneCurveEditor::settingsCommitted, this, &CalibrationOverlay::settingsCommitted);
    connect(m_editor, &ToneCurveEditor::layoutChanged, this, [this]() {
        if (m_contentPanel) {
            m_contentPanel->adjustSize();
            repositionPanel();
        }
    });
    connect(m_perceptualColor, &QCheckBox::toggled, this, &CalibrationOverlay::settingsChanged);
    connect(m_aiEnhanced, &QCheckBox::toggled, this, &CalibrationOverlay::settingsChanged);
    connect(m_aiStrength, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &CalibrationOverlay::settingsChanged);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &CalibrationOverlay::onConfirm);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &CalibrationOverlay::onCancel);

    m_contentPanel->adjustSize();
    repositionPanel();
}

void CalibrationOverlay::setConfig(const KSharedConfigPtr &config)
{
    m_editor->setConfig(config);
}

void CalibrationOverlay::setHdrLimits(int minPeakNits, int maxDisplayNits)
{
    m_editor->setHdrLimits(minPeakNits, maxDisplayNits);
    m_editor->setOverlayLuminanceFactor(0.8f);
}

void CalibrationOverlay::setPerceptualColorEnabled(bool enabled)
{
    m_perceptualColor->setChecked(enabled);
}

bool CalibrationOverlay::perceptualColorEnabled() const
{
    return m_perceptualColor->isChecked();
}

void CalibrationOverlay::setValues(const AutoHdr::CalibrationSettings &settings)
{
    m_editor->setValues(settings.maxNits, settings.referenceNits, settings.sdrMaxPoint, settings.toneCurvePoints,
                        settings.blackPoint, settings.toneCurvePreset, settings.toneCurveUserPresetId,
                        settings.gamutExpansion, settings.colorIntensity);
    m_aiEnhanced->setChecked(settings.aiEnhanced);
    m_aiStrength->setValue(settings.aiStrength * 100.0);
}

AutoHdr::CalibrationSettings CalibrationOverlay::currentValues() const
{
    AutoHdr::CalibrationSettings settings;
    float peakNits = 0.0f;
    float referenceNits = 0.0f;
    QPointF sdrMaxPoint;
    QVector<QPointF> intermediatePoints;
    float blackPoint = 0.0f;
    AutoHdr::ToneCurvePreset preset = AutoHdr::ToneCurvePreset::Linear;
    QString userPresetId;
    float gamutExpansion = 1.5f;
    float colorIntensity = 0.33f;

    m_editor->getValues(peakNits, referenceNits, sdrMaxPoint, intermediatePoints, blackPoint, preset, userPresetId,
                        gamutExpansion, colorIntensity);

    settings.maxNits = peakNits;
    settings.referenceNits = referenceNits;
    settings.sdrMaxPoint = sdrMaxPoint;
    settings.toneCurvePoints = intermediatePoints;
    settings.blackPoint = blackPoint;
    settings.toneCurvePreset = preset;
    settings.toneCurveUserPresetId = userPresetId;
    settings.gamutExpansion = gamutExpansion;
    settings.colorIntensity = colorIntensity;
    settings.aiEnhanced = m_aiEnhanced->isChecked();
    settings.aiStrength =
        AutoHdr::clampAiStrength(static_cast<float>(m_aiStrength->value() / 100.0));
    return settings;
}

QRect CalibrationOverlay::panelBlurRegion() const
{
    return m_contentPanel ? m_contentPanel->geometry() : QRect();
}

void CalibrationOverlay::repositionPanel()
{
    if (!m_contentPanel) {
        return;
    }
    m_contentPanel->move((width() - m_contentPanel->width()) / 2,
                         height() - m_contentPanel->height() - 24);
}

void CalibrationOverlay::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    repositionPanel();
}

void CalibrationOverlay::keyPressEvent(QKeyEvent *event)
{
    if (m_editor && m_editor->isPresetPromptOpen()) {
        QWidget::keyPressEvent(event);
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        onCancel();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (!(event->modifiers() & Qt::ShiftModifier)) {
            onConfirm();
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

void CalibrationOverlay::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
}

void CalibrationOverlay::onConfirm()
{
    Q_EMIT confirmed();
}

void CalibrationOverlay::onCancel()
{
    Q_EMIT cancelled();
}
