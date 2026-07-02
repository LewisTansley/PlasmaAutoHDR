/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "calibration_overlay.h"

#include <QDialogButtonBox>
#include <QKeyEvent>
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

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, m_contentPanel);
    if (QPushButton *confirm = m_buttons->button(QDialogButtonBox::Ok)) {
        confirm->setText(tr("Confirm"));
    }
    panelLayout->addWidget(m_buttons);

    connect(m_editor, &ToneCurveEditor::settingsChanged, this, &CalibrationOverlay::settingsChanged);
    connect(m_editor, &ToneCurveEditor::settingsCommitted, this, &CalibrationOverlay::settingsCommitted);
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

void CalibrationOverlay::setValues(const AutoHdr::CalibrationSettings &settings)
{
    m_editor->setValues(settings.maxNits, settings.referenceNits, settings.sdrMaxPoint, settings.toneCurvePoints,
                        settings.blackPoint, settings.toneCurvePreset, settings.toneCurveUserPresetId,
                        settings.vibrance, settings.gamutExpansion);
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
    float vibrance = 0.0f;
    float gamutExpansion = 1.5f;

    m_editor->getValues(peakNits, referenceNits, sdrMaxPoint, intermediatePoints, blackPoint, preset, userPresetId,
                        vibrance, gamutExpansion);

    settings.maxNits = peakNits;
    settings.referenceNits = referenceNits;
    settings.sdrMaxPoint = sdrMaxPoint;
    settings.toneCurvePoints = intermediatePoints;
    settings.blackPoint = blackPoint;
    settings.toneCurvePreset = preset;
    settings.toneCurveUserPresetId = userPresetId;
    settings.vibrance = vibrance;
    settings.gamutExpansion = gamutExpansion;
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
