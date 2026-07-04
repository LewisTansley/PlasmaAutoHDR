/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "autohdr_config.h"
#include "ui/tone_curve_editor.h"

#include <KConfigGroup>
#include <KCModule>
#include <KLocalizedString>
#include <KPluginFactory>
#include <KSharedConfig>

#include <QCheckBox>
#include <QComboBox>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

struct HdrDisplayLimits {
    int referenceNits = 100;
    int maxDisplayNits = 1000;
};

HdrDisplayLimits readHdrDisplayLimits()
{
    const KConfigGroup hdrGroup(KSharedConfig::openConfig(QStringLiteral("kwinrc")), QStringLiteral("Windows_HDR"));
    HdrDisplayLimits limits;
    limits.referenceNits = qMax(1, qRound(hdrGroup.readEntry("Reference", 100.0f)));
    limits.maxDisplayNits = qMax(limits.referenceNits + 1, qRound(hdrGroup.readEntry("MaxLuminance", 1000.0f)));

    const QString outputConfigPath =
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/kwinoutputconfig.json");
    QFile outputConfigFile(outputConfigPath);
    if (outputConfigFile.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(outputConfigFile.readAll());
        for (const QJsonValue &screenValue : doc.array()) {
            const QJsonArray outputs = screenValue.toObject().value(QStringLiteral("data")).toArray();
            for (const QJsonValue &outputValue : outputs) {
                const QJsonObject output = outputValue.toObject();
                if (!output.value(QStringLiteral("highDynamicRange")).toBool(false)) {
                    continue;
                }
                const int peakOverride = output.value(QStringLiteral("maxPeakBrightnessOverride")).toInt(0);
                if (peakOverride > limits.maxDisplayNits) {
                    limits.maxDisplayNits = peakOverride;
                }
            }
        }
    }

    return limits;
}

void notifyKWin()
{
    QDBusConnection bus = QDBusConnection::sessionBus();

    QDBusMessage reloadMessage = QDBusMessage::createMethodCall(QStringLiteral("org.kde.kwin.effect.autohdr"),
                                                                QStringLiteral("/autohdr"),
                                                                QStringLiteral("org.kde.kwin.effect.autohdr"),
                                                                QStringLiteral("reloadSettings"));
    bus.call(reloadMessage);

    QDBusMessage reconfigureMessage = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"),
                                                                     QStringLiteral("/Effects"),
                                                                     QStringLiteral("org.kde.kwin.Effects"),
                                                                     QStringLiteral("reconfigureEffect"));
    reconfigureMessage.setArguments({QStringLiteral("kwin4_effect_autohdr")});
    bus.call(reconfigureMessage);
}

} // namespace

class AutoHdrEffectKcm : public KCModule
{
    Q_OBJECT

public:
    AutoHdrEffectKcm(QObject *parent, const KPluginMetaData &data)
        : KCModule(parent, data)
        , m_config(AutoHdr::openConfig())
    {
        auto *layout = new QVBoxLayout(widget());

        auto *defaultsGroup = new QGroupBox(i18n("Global Defaults"), widget());
        auto *defaultsOuterLayout = new QVBoxLayout(defaultsGroup);

        m_toneCurveEditor = new ToneCurveEditor(defaultsGroup);
        defaultsOuterLayout->addWidget(m_toneCurveEditor);

        layout->addWidget(defaultsGroup);

        auto *smoothingGroup = new QGroupBox(i18n("HDR Output Smoothing"), widget());
        auto *smoothingLayout = new QFormLayout(smoothingGroup);

        m_curveAntialias = new QDoubleSpinBox(smoothingGroup);
        m_curveAntialias->setRange(0.0, 100.0);
        m_curveAntialias->setSuffix(QStringLiteral(" %"));
        m_curveAntialias->setDecimals(0);
        m_curveAntialias->setSingleStep(5.0);
        m_curveAntialias->setToolTip(
            i18n("Reduces harsh tone-curve transitions on anti-aliased edges such as text and thin UI lines."));
        smoothingLayout->addRow(i18n("Curve anti-aliasing:"), m_curveAntialias);

        m_antiAliasingQuality = new QComboBox(smoothingGroup);
        m_antiAliasingQuality->addItem(i18n("Low — 4-neighbor, tuned weights"), 0);
        m_antiAliasingQuality->addItem(i18n("Medium — 8-neighbor (+4 diagonals)"), 1);
        m_antiAliasingQuality->addItem(i18n("High — 12-sample wide curve pooling"), 2);
        m_antiAliasingQuality->setToolTip(
            i18n("Controls how many neighboring pixels are sampled for curve-domain anti-aliasing. "
                 "Higher settings improve thin lines and diagonal edges at a small GPU cost."));
        smoothingLayout->addRow(i18n("Anti-aliasing quality:"), m_antiAliasingQuality);

        m_highlightSoftness = new QDoubleSpinBox(smoothingGroup);
        m_highlightSoftness->setRange(0.0, 100.0);
        m_highlightSoftness->setSuffix(QStringLiteral(" %"));
        m_highlightSoftness->setDecimals(0);
        m_highlightSoftness->setSingleStep(5.0);
        m_highlightSoftness->setToolTip(
            i18n("Softens the transition into display peak luminance instead of hard clipping bright highlights."));
        smoothingLayout->addRow(i18n("Highlight softness:"), m_highlightSoftness);

        layout->addWidget(smoothingGroup);

        m_perceptualColor = new QCheckBox(
            i18n("Use perceptual luminance/chroma separation"), widget());
        m_perceptualColor->setToolTip(
            i18n("Off uses legacy RGB-coupled tone mapping and peak limiting. "
                 "On applies PQ perceptual remapping in XYZ; color intensity blends "
                 "Y-only vs full-XYZ PQ boost."));
        layout->addWidget(m_perceptualColor);

        auto *aiGroup = new QGroupBox(i18n("AI-Enhanced HDR"), widget());
        auto *aiLayout = new QFormLayout(aiGroup);

        m_aiEnhanced = new QCheckBox(i18n("Enable content-aware detail recovery"), aiGroup);
        m_aiEnhanced->setToolTip(
            i18n("Uses a low-resolution guidance map (GLSL on AMD/Mesa by default, "
                 "optional ONNX Runtime with Vulkan/CPU) to recover shadow and highlight "
                 "micro-detail, add perceptual depth in flat midtones, and expand lights, "
                 "sky, and speculars."));
        aiLayout->addRow(QString(), m_aiEnhanced);

        m_aiStrength = new QDoubleSpinBox(aiGroup);
        m_aiStrength->setRange(0.0, 100.0);
        m_aiStrength->setSuffix(QStringLiteral(" %"));
        m_aiStrength->setDecimals(0);
        m_aiStrength->setSingleStep(5.0);
        m_aiStrength->setToolTip(
            i18n("How strongly guidance applies shadow/highlight detail, depth, and highlight expansion."));
        aiLayout->addRow(i18n("AI strength:"), m_aiStrength);

        m_aiQuality = new QComboBox(aiGroup);
        m_aiQuality->addItem(i18n("Performance — 1/4 map, every frame"),
                             static_cast<int>(AutoHdr::AiQuality::Performance));
        m_aiQuality->addItem(i18n("Balanced — 1/2 map, every frame"),
                             static_cast<int>(AutoHdr::AiQuality::Balanced));
        m_aiQuality->addItem(i18n("Quality — full map, every frame"),
                             static_cast<int>(AutoHdr::AiQuality::Quality));
        m_aiQuality->setToolTip(
            i18n("Guidance map resolution. Higher tiers cost more GPU time but reduce "
                 "blockiness and keep highlights locked to motion."));
        aiLayout->addRow(i18n("AI quality:"), m_aiQuality);

        m_aiBackend = new QComboBox(aiGroup);
        m_aiBackend->addItem(i18n("Auto — ONNX when available, else GLSL"),
                             static_cast<int>(AutoHdr::AiBackend::Auto));
        m_aiBackend->addItem(i18n("ONNX Runtime (Vulkan/CPU)"),
                             static_cast<int>(AutoHdr::AiBackend::Onnx));
        m_aiBackend->addItem(i18n("GLSL only"), static_cast<int>(AutoHdr::AiBackend::GlslOnly));
        m_aiBackend->setToolTip(
            i18n("GLSL and ONNX both use the guidance_v1 detail model on the GPU (same look, any quality tier). "
                 "Set AUTOHDR_ONNX_FORCE_ORT=1 to force real ONNX Runtime readback (slower, for future models)."));
        aiLayout->addRow(i18n("AI backend:"), m_aiBackend);

        auto *aiBackendNote = new QLabel(
            i18n("ONNX v1 runs on the GPU by default (onnx-style-gpu). "
                 "Force ORT readback only if you need a non-v1 model."),
            aiGroup);
        aiBackendNote->setWordWrap(true);
        aiBackendNote->setStyleSheet(QStringLiteral("color: palette(mid);"));
        aiLayout->addRow(QString(), aiBackendNote);

        layout->addWidget(aiGroup);

        auto *chromaGroup = new QGroupBox(i18n("AI Chroma Inference"), widget());
        auto *chromaLayout = new QFormLayout(chromaGroup);

        m_aiChromaEnabled = new QCheckBox(i18n("Enable AI chroma refinement"), chromaGroup);
        m_aiChromaEnabled->setToolTip(
            i18n("Infers color corrections separately from luminance: reduces chroma banding, "
                 "recovers highlight color, and adapts perceptual color intensity per pixel."));
        chromaLayout->addRow(QString(), m_aiChromaEnabled);

        m_aiChromaStrength = new QDoubleSpinBox(chromaGroup);
        m_aiChromaStrength->setRange(0.0, 100.0);
        m_aiChromaStrength->setSuffix(QStringLiteral(" %"));
        m_aiChromaStrength->setDecimals(0);
        m_aiChromaStrength->setSingleStep(5.0);
        m_aiChromaStrength->setToolTip(
            i18n("How strongly the chroma map applies color refinement (Y-locked)."));
        chromaLayout->addRow(i18n("Chroma strength:"), m_aiChromaStrength);

        layout->addWidget(chromaGroup);

        m_autoActivate = new QCheckBox(i18n("Automatically apply shader to calibrated applications"), widget());
        layout->addWidget(m_autoActivate);

        auto *appsGroup = new QGroupBox(i18n("Calibrated Applications"), widget());
        auto *appsLayout = new QVBoxLayout(appsGroup);

        m_appsTable = new QTableWidget(0, 3, appsGroup);
        m_appsTable->setHorizontalHeaderLabels({i18n("Application"), i18n("Auto-activate"), i18n("")});
        m_appsTable->horizontalHeader()->setStretchLastSection(false);
        m_appsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_appsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_appsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_appsTable->setSelectionMode(QAbstractItemView::NoSelection);
        m_appsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        appsLayout->addWidget(m_appsTable);

        layout->addWidget(appsGroup);

        connect(m_autoActivate, &QCheckBox::toggled, this, &KCModule::markAsChanged);
        connect(m_perceptualColor, &QCheckBox::toggled, this, &KCModule::markAsChanged);
        connect(m_aiEnhanced, &QCheckBox::toggled, this, &KCModule::markAsChanged);
        connect(m_toneCurveEditor, &ToneCurveEditor::settingsChanged, this, &KCModule::markAsChanged);
        connect(m_curveAntialias, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &KCModule::markAsChanged);
        connect(m_antiAliasingQuality, qOverload<int>(&QComboBox::currentIndexChanged), this, &KCModule::markAsChanged);
        connect(m_highlightSoftness, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                &KCModule::markAsChanged);
        connect(m_aiStrength, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &KCModule::markAsChanged);
        connect(m_aiQuality, qOverload<int>(&QComboBox::currentIndexChanged), this, &KCModule::markAsChanged);
        connect(m_aiBackend, qOverload<int>(&QComboBox::currentIndexChanged), this, &KCModule::markAsChanged);
        connect(m_aiChromaEnabled, &QCheckBox::toggled, this, &KCModule::markAsChanged);
        connect(m_aiChromaStrength, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &KCModule::markAsChanged);
    }

    void load() override
    {
        KCModule::load();

        const HdrDisplayLimits limits = readHdrDisplayLimits();
        m_toneCurveEditor->setHdrLimits(limits.referenceNits + 1, limits.maxDisplayNits);

        m_config->reparseConfiguration();
        const AutoHdr::CalibrationSettings globals =
            AutoHdr::loadGlobalSettings(m_config, static_cast<float>(limits.maxDisplayNits));

        m_toneCurveEditor->setConfig(m_config);

        m_toneCurveEditor->setValues(globals.maxNits, globals.referenceNits, globals.sdrMaxPoint,
                                     globals.toneCurvePoints, globals.blackPoint, globals.toneCurvePreset,
                                     globals.toneCurveUserPresetId, globals.gamutExpansion,
                                     globals.colorIntensity);

        const AutoHdr::GeneralSettings general = AutoHdr::loadGeneralSettings(m_config);
        m_autoActivate->setChecked(general.autoActivateCalibrated);
        m_perceptualColor->setChecked(general.perceptualColorEnabled);
        m_curveAntialias->setValue(general.curveAntialiasStrength * 100.0);
        const int aaQualityIndex = m_antiAliasingQuality->findData(general.antiAliasingQuality);
        m_antiAliasingQuality->setCurrentIndex(aaQualityIndex >= 0 ? aaQualityIndex : 0);
        m_highlightSoftness->setValue(general.highlightSoftness * 100.0);
        m_aiEnhanced->setChecked(general.aiEnhanced);
        m_aiStrength->setValue(general.aiStrength * 100.0);
        const int aiQualityIndex = m_aiQuality->findData(static_cast<int>(general.aiQuality));
        m_aiQuality->setCurrentIndex(aiQualityIndex >= 0 ? aiQualityIndex : 1);
        const int aiBackendIndex = m_aiBackend->findData(static_cast<int>(general.aiBackend));
        m_aiBackend->setCurrentIndex(aiBackendIndex >= 0 ? aiBackendIndex : 0);
        m_aiChromaEnabled->setChecked(general.aiChromaEnabled);
        m_aiChromaStrength->setValue(general.aiChromaStrength * 100.0);

        rebuildAppsTable();
    }

    void save() override
    {
        const HdrDisplayLimits limits = readHdrDisplayLimits();

        AutoHdr::CalibrationSettings globals =
            AutoHdr::loadGlobalSettings(m_config, static_cast<float>(limits.maxDisplayNits));

        float peakNits = 0.0f;
        float referenceNits = 0.0f;
        float blackPoint = 0.0f;
        float gamutExpansion = 1.5f;
        float colorIntensity = 0.33f;
        QPointF sdrMaxPoint;
        QVector<QPointF> intermediatePoints;
        AutoHdr::ToneCurvePreset toneCurvePreset = AutoHdr::ToneCurvePreset::Linear;
        QString toneCurveUserPresetId;
        m_toneCurveEditor->getValues(peakNits, referenceNits, sdrMaxPoint, intermediatePoints, blackPoint,
                                     toneCurvePreset, toneCurveUserPresetId, gamutExpansion, colorIntensity);
        globals.gamutExpansion = gamutExpansion;
        globals.colorIntensity = colorIntensity;
        globals.maxNits = peakNits;
        globals.referenceNits = referenceNits;
        globals.sdrMaxPoint = sdrMaxPoint;
        globals.toneCurvePoints = intermediatePoints;
        globals.blackPoint = blackPoint;
        globals.toneCurvePreset = toneCurvePreset;
        globals.toneCurveUserPresetId = toneCurveUserPresetId;

        AutoHdr::sanitizeCalibrationSettings(globals, static_cast<float>(limits.referenceNits),
                                             static_cast<float>(limits.maxDisplayNits), m_config);
        AutoHdr::saveGlobalSettings(m_config, globals);

        AutoHdr::GeneralSettings general;
        general.autoActivateCalibrated = m_autoActivate->isChecked();
        general.perceptualColorEnabled = m_perceptualColor->isChecked();
        general.curveAntialiasStrength =
            AutoHdr::clampCurveAntialiasStrength(static_cast<float>(m_curveAntialias->value() / 100.0));
        general.antiAliasingQuality = AutoHdr::clampAntiAliasingQuality(
            m_antiAliasingQuality->currentData().toInt());
        general.highlightSoftness =
            AutoHdr::clampHighlightSoftness(static_cast<float>(m_highlightSoftness->value() / 100.0));
        general.aiEnhanced = m_aiEnhanced->isChecked();
        general.aiStrength =
            AutoHdr::clampAiStrength(static_cast<float>(m_aiStrength->value() / 100.0));
        general.aiQuality = AutoHdr::clampAiQuality(m_aiQuality->currentData().toInt());
        general.aiBackend = AutoHdr::clampAiBackend(m_aiBackend->currentData().toInt());
        general.aiChromaEnabled = m_aiChromaEnabled->isChecked();
        general.aiChromaStrength =
            AutoHdr::clampAiChromaStrength(static_cast<float>(m_aiChromaStrength->value() / 100.0));
        AutoHdr::saveGeneralSettings(m_config, general);

        saveAppsTable();

        KCModule::save();
        notifyKWin();
    }

private:
    void rebuildAppsTable()
    {
        m_appsTable->setRowCount(0);
        m_appKeys.clear();

        for (const QString &key : AutoHdr::listCalibratedApps(m_config)) {
            const std::optional<AutoHdr::AppProfile> profile = AutoHdr::loadAppProfile(m_config, key);
            if (!profile) {
                continue;
            }

            const int row = m_appsTable->rowCount();
            m_appsTable->insertRow(row);
            m_appKeys.append(key);

            auto *nameItem = new QTableWidgetItem(profile->metadata.displayName);
            nameItem->setToolTip(profile->metadata.windowClass);
            m_appsTable->setItem(row, 0, nameItem);

            auto *autoActivate = new QCheckBox(m_appsTable);
            autoActivate->setChecked(profile->metadata.autoActivate);
            connect(autoActivate, &QCheckBox::toggled, this, &KCModule::markAsChanged);
            m_appsTable->setCellWidget(row, 1, autoActivate);

            auto *deleteButton = new QPushButton(i18n("Delete"), m_appsTable);
            connect(deleteButton, &QPushButton::clicked, this, [this, key]() {
                m_pendingDeletes.insert(key);
                markAsChanged();
                for (int row = 0; row < m_appsTable->rowCount(); ++row) {
                    if (row < m_appKeys.size() && m_appKeys.at(row) == key) {
                        m_appsTable->removeRow(row);
                        m_appKeys.removeAt(row);
                        break;
                    }
                }
            });
            m_appsTable->setCellWidget(row, 2, deleteButton);
        }
    }

    void saveAppsTable()
    {
        for (const QString &key : std::as_const(m_pendingDeletes)) {
            AutoHdr::deleteAppProfile(m_config, key);
        }
        m_pendingDeletes.clear();

        for (int row = 0; row < m_appsTable->rowCount(); ++row) {
            if (row >= m_appKeys.size()) {
                break;
            }
            const QString key = m_appKeys.at(row);
            const std::optional<AutoHdr::AppProfile> profile = AutoHdr::loadAppProfile(m_config, key);
            if (!profile) {
                continue;
            }

            AutoHdr::AppProfile updated = *profile;
            if (auto *autoActivate = qobject_cast<QCheckBox *>(m_appsTable->cellWidget(row, 1))) {
                updated.metadata.autoActivate = autoActivate->isChecked();
            }
            AutoHdr::saveAppProfile(m_config, updated);
        }
    }

    KSharedConfigPtr m_config;
    QCheckBox *m_autoActivate = nullptr;
    QCheckBox *m_perceptualColor = nullptr;
    QCheckBox *m_aiEnhanced = nullptr;
    QDoubleSpinBox *m_aiStrength = nullptr;
    QComboBox *m_aiQuality = nullptr;
    QComboBox *m_aiBackend = nullptr;
    QCheckBox *m_aiChromaEnabled = nullptr;
    QDoubleSpinBox *m_aiChromaStrength = nullptr;
    QDoubleSpinBox *m_curveAntialias = nullptr;
    QComboBox *m_antiAliasingQuality = nullptr;
    QDoubleSpinBox *m_highlightSoftness = nullptr;
    ToneCurveEditor *m_toneCurveEditor = nullptr;
    QTableWidget *m_appsTable = nullptr;
    QStringList m_appKeys;
    QSet<QString> m_pendingDeletes;
};

K_PLUGIN_CLASS_WITH_JSON(AutoHdrEffectKcm, "kwin4_effect_autohdr_config.json")

#include "autohdr_effect_kcm.moc"
