/*
    SPDX-FileCopyrightText: 2026 Luu
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "autohdr_config.h"
#include "tone_curve_editor.h"

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
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
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

QJsonObject profileToJson(const AutoHdr::AppProfile &profile)
{
    QJsonObject root;
    QJsonObject metadata;
    metadata.insert(QStringLiteral("key"), profile.metadata.key);
    metadata.insert(QStringLiteral("displayName"), profile.metadata.displayName);
    metadata.insert(QStringLiteral("windowClass"), profile.metadata.windowClass);
    metadata.insert(QStringLiteral("resourceClass"), profile.metadata.resourceClass);
    metadata.insert(QStringLiteral("desktopFile"), profile.metadata.desktopFile);
    metadata.insert(QStringLiteral("autoActivate"), profile.metadata.autoActivate);
    root.insert(QStringLiteral("metadata"), metadata);

    const AutoHdr::CalibrationSettings &s = profile.settings;
    QJsonObject settings;
    settings.insert(QStringLiteral("MaxNits"), s.maxNits);
    settings.insert(QStringLiteral("GamutExpansion"), static_cast<double>(s.gamutExpansion));
    settings.insert(QStringLiteral("BlackPoint"), static_cast<double>(s.blackPoint));
    settings.insert(QStringLiteral("Vibrance"), static_cast<double>(s.vibrance));
    settings.insert(QStringLiteral("ReferenceNits"), static_cast<double>(s.referenceNits));
    settings.insert(QStringLiteral("ChromaCompensation"), static_cast<double>(s.chromaCompensation));
    settings.insert(QStringLiteral("HighlightRolloff"), static_cast<double>(s.highlightRolloff));
    settings.insert(QStringLiteral("GamutMappingStrength"), static_cast<double>(s.gamutMappingStrength));
    settings.insert(QStringLiteral("SdrMaxPoint"), AutoHdr::formatSdrMaxPoint(s.sdrMaxPoint));
    settings.insert(QStringLiteral("ToneCurvePoints"), AutoHdr::formatToneCurvePoints(s.toneCurvePoints));
    settings.insert(QStringLiteral("ToneCurvePreset"), AutoHdr::presetToString(s.toneCurvePreset));
    settings.insert(QStringLiteral("ToneCurveUserPresetId"), s.toneCurveUserPresetId);
    root.insert(QStringLiteral("settings"), settings);
    return root;
}

std::optional<AutoHdr::AppProfile> profileFromJson(const QJsonObject &root)
{
    if (!root.contains(QStringLiteral("metadata")) || !root.contains(QStringLiteral("settings"))) {
        return std::nullopt;
    }

    AutoHdr::AppProfile profile;
    const QJsonObject metadata = root.value(QStringLiteral("metadata")).toObject();
    profile.metadata.key = metadata.value(QStringLiteral("key")).toString();
    profile.metadata.displayName = metadata.value(QStringLiteral("displayName")).toString();
    profile.metadata.windowClass = metadata.value(QStringLiteral("windowClass")).toString();
    profile.metadata.resourceClass = metadata.value(QStringLiteral("resourceClass")).toString();
    profile.metadata.desktopFile = metadata.value(QStringLiteral("desktopFile")).toString();
    profile.metadata.autoActivate = metadata.value(QStringLiteral("autoActivate")).toBool(true);

    if (profile.metadata.key.isEmpty()) {
        return std::nullopt;
    }

    const QJsonObject settings = root.value(QStringLiteral("settings")).toObject();
    profile.settings.maxNits = static_cast<float>(settings.value(QStringLiteral("MaxNits")).toDouble(1000.0));
    profile.settings.gamutExpansion = static_cast<float>(settings.value(QStringLiteral("GamutExpansion")).toDouble(1.5));
    profile.settings.blackPoint = static_cast<float>(settings.value(QStringLiteral("BlackPoint")).toDouble(0.0));
    profile.settings.vibrance = static_cast<float>(settings.value(QStringLiteral("Vibrance")).toDouble(0.0));
    profile.settings.referenceNits = static_cast<float>(settings.value(QStringLiteral("ReferenceNits")).toDouble(203.0));
    profile.settings.chromaCompensation =
        static_cast<float>(settings.value(QStringLiteral("ChromaCompensation")).toDouble(0.0));
    profile.settings.highlightRolloff =
        static_cast<float>(settings.value(QStringLiteral("HighlightRolloff")).toDouble(0.0));
    profile.settings.gamutMappingStrength =
        static_cast<float>(settings.value(QStringLiteral("GamutMappingStrength")).toDouble(0.0));
    profile.settings.sdrMaxPoint =
        AutoHdr::parseSdrMaxPoint(settings.value(QStringLiteral("SdrMaxPoint")).toString(), QPointF());
    profile.settings.toneCurvePoints =
        AutoHdr::parseToneCurvePoints(settings.value(QStringLiteral("ToneCurvePoints")).toString());
    profile.settings.toneCurvePreset =
        AutoHdr::presetFromString(settings.value(QStringLiteral("ToneCurvePreset")).toString());
    profile.settings.toneCurveUserPresetId = settings.value(QStringLiteral("ToneCurveUserPresetId")).toString();
    return profile;
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

        auto *processingGroup = new QGroupBox(i18n("Processing"), widget());
        auto *processingLayout = new QFormLayout(processingGroup);

        m_processingQuality = new QComboBox(processingGroup);
        m_processingQuality->addItem(i18n("Fast"), 0);
        m_processingQuality->addItem(i18n("Balanced"), 1);
        m_processingQuality->addItem(i18n("Quality"), 2);
        processingLayout->addRow(i18n("Processing quality:"), m_processingQuality);

        m_preferFloatCapture = new QCheckBox(i18n("Use float-precision window capture (recommended)"), processingGroup);
        processingLayout->addRow(m_preferFloatCapture);

        m_debandStrength = new QDoubleSpinBox(processingGroup);
        m_debandStrength->setRange(0.0, 1.0);
        m_debandStrength->setSingleStep(0.05);
        processingLayout->addRow(i18n("Deband strength:"), m_debandStrength);

        m_ditherStrength = new QDoubleSpinBox(processingGroup);
        m_ditherStrength->setRange(0.0, 1.0);
        m_ditherStrength->setDecimals(5);
        m_ditherStrength->setSingleStep(0.001);
        processingLayout->addRow(i18n("Dither strength:"), m_ditherStrength);

        m_postCurveDebandStrength = new QDoubleSpinBox(processingGroup);
        m_postCurveDebandStrength->setRange(0.0, 1.0);
        m_postCurveDebandStrength->setSingleStep(0.05);
        processingLayout->addRow(i18n("Post-curve deband:"), m_postCurveDebandStrength);

        m_spatialHighlightRecovery = new QCheckBox(
            i18n("Enable spatial highlight recovery (experimental)"), processingGroup);
        processingLayout->addRow(m_spatialHighlightRecovery);

        layout->addWidget(processingGroup);

        m_autoActivate = new QCheckBox(i18n("Automatically apply shader to calibrated applications"), widget());
        layout->addWidget(m_autoActivate);

        auto *appsGroup = new QGroupBox(i18n("Calibrated Applications"), widget());
        auto *appsLayout = new QVBoxLayout(appsGroup);

        auto *appsButtons = new QHBoxLayout();
        auto *importButton = new QPushButton(i18n("Import profile…"), appsGroup);
        auto *exportButton = new QPushButton(i18n("Export profile…"), appsGroup);
        appsButtons->addWidget(importButton);
        appsButtons->addWidget(exportButton);
        appsButtons->addStretch();
        appsLayout->addLayout(appsButtons);

        m_appsTable = new QTableWidget(0, 3, appsGroup);
        m_appsTable->setHorizontalHeaderLabels({i18n("Application"), i18n("Auto-activate"), i18n("")});
        m_appsTable->horizontalHeader()->setStretchLastSection(false);
        m_appsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_appsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_appsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_appsTable->setSelectionMode(QAbstractItemView::SingleSelection);
        m_appsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        appsLayout->addWidget(m_appsTable);

        layout->addWidget(appsGroup);

        connect(m_autoActivate, &QCheckBox::toggled, this, &KCModule::markAsChanged);
        connect(m_toneCurveEditor, &ToneCurveEditor::settingsChanged, this, &KCModule::markAsChanged);
        connect(m_processingQuality, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &KCModule::markAsChanged);
        connect(m_preferFloatCapture, &QCheckBox::toggled, this, &KCModule::markAsChanged);
        connect(m_debandStrength, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &KCModule::markAsChanged);
        connect(m_ditherStrength, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &KCModule::markAsChanged);
        connect(m_postCurveDebandStrength, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                &KCModule::markAsChanged);
        connect(m_spatialHighlightRecovery, &QCheckBox::toggled, this, &KCModule::markAsChanged);
        connect(importButton, &QPushButton::clicked, this, &AutoHdrEffectKcm::importProfile);
        connect(exportButton, &QPushButton::clicked, this, &AutoHdrEffectKcm::exportProfile);
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
                                     globals.toneCurveUserPresetId, globals.vibrance, globals.gamutExpansion,
                                     globals.chromaCompensation, globals.highlightRolloff,
                                     globals.gamutMappingStrength);

        m_autoActivate->setChecked(AutoHdr::loadGeneralSettings(m_config).autoActivateCalibrated);

        const AutoHdr::GeneralSettings general = AutoHdr::loadGeneralSettings(m_config);
        m_autoActivate->setChecked(general.autoActivateCalibrated);
        const int qualityIndex = m_processingQuality->findData(general.processingQuality);
        m_processingQuality->setCurrentIndex(qualityIndex >= 0 ? qualityIndex : 0);
        m_preferFloatCapture->setChecked(general.preferFloatCapture);
        m_debandStrength->setValue(general.debandStrength);
        m_ditherStrength->setValue(general.ditherStrength);
        m_postCurveDebandStrength->setValue(general.postCurveDebandStrength);
        m_spatialHighlightRecovery->setChecked(general.spatialHighlightRecovery);

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
        float vibrance = 0.0f;
        float gamutExpansion = 1.5f;
        float chromaCompensation = 0.0f;
        float highlightRolloff = 0.0f;
        float gamutMappingStrength = 0.0f;
        QPointF sdrMaxPoint;
        QVector<QPointF> intermediatePoints;
        AutoHdr::ToneCurvePreset toneCurvePreset = AutoHdr::ToneCurvePreset::Linear;
        QString toneCurveUserPresetId;
        m_toneCurveEditor->getValues(peakNits, referenceNits, sdrMaxPoint, intermediatePoints, blackPoint,
                                     toneCurvePreset, toneCurveUserPresetId, vibrance, gamutExpansion,
                                     chromaCompensation, highlightRolloff, gamutMappingStrength);
        globals.vibrance = vibrance;
        globals.gamutExpansion = gamutExpansion;
        globals.chromaCompensation = chromaCompensation;
        globals.highlightRolloff = highlightRolloff;
        globals.gamutMappingStrength = gamutMappingStrength;
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
        general.processingQuality = m_processingQuality->currentData().toInt();
        general.preferFloatCapture = m_preferFloatCapture->isChecked();
        general.debandStrength = static_cast<float>(m_debandStrength->value());
        general.ditherStrength = static_cast<float>(m_ditherStrength->value());
        general.postCurveDebandStrength = static_cast<float>(m_postCurveDebandStrength->value());
        general.spatialHighlightRecovery = m_spatialHighlightRecovery->isChecked();
        AutoHdr::saveGeneralSettings(m_config, general);

        saveAppsTable();

        KCModule::save();
        notifyKWin();
    }

private Q_SLOTS:
    void importProfile()
    {
        const QString path =
            QFileDialog::getOpenFileName(widget(), i18n("Import AutoHDR Profile"), QString(), i18n("JSON (*.json)"));
        if (path.isEmpty()) {
            return;
        }

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(widget(), i18n("Import Failed"), i18n("Could not open the selected file."));
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        const std::optional<AutoHdr::AppProfile> profile = profileFromJson(doc.object());
        if (!profile) {
            QMessageBox::warning(widget(), i18n("Import Failed"), i18n("The file is not a valid AutoHDR profile."));
            return;
        }

        AutoHdr::saveAppProfile(m_config, *profile);
        rebuildAppsTable();
        markAsChanged();
    }

    void exportProfile()
    {
        const int row = m_appsTable->currentRow();
        if (row < 0 || row >= m_appKeys.size()) {
            QMessageBox::information(widget(), i18n("Export Profile"), i18n("Select an application row first."));
            return;
        }

        const std::optional<AutoHdr::AppProfile> profile = AutoHdr::loadAppProfile(m_config, m_appKeys.at(row));
        if (!profile) {
            return;
        }

        const QString path = QFileDialog::getSaveFileName(widget(), i18n("Export AutoHDR Profile"),
                                                          profile->metadata.displayName + QStringLiteral(".json"),
                                                          i18n("JSON (*.json)"));
        if (path.isEmpty()) {
            return;
        }

        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(widget(), i18n("Export Failed"), i18n("Could not write the selected file."));
            return;
        }

        file.write(QJsonDocument(profileToJson(*profile)).toJson(QJsonDocument::Indented));
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
    QComboBox *m_processingQuality = nullptr;
    QCheckBox *m_preferFloatCapture = nullptr;
    QDoubleSpinBox *m_debandStrength = nullptr;
    QDoubleSpinBox *m_ditherStrength = nullptr;
    QDoubleSpinBox *m_postCurveDebandStrength = nullptr;
    QCheckBox *m_spatialHighlightRecovery = nullptr;
    ToneCurveEditor *m_toneCurveEditor = nullptr;
    QTableWidget *m_appsTable = nullptr;
    QStringList m_appKeys;
    QSet<QString> m_pendingDeletes;
};

K_PLUGIN_CLASS_WITH_JSON(AutoHdrEffectKcm, "kwin4_effect_autohdr_config.json")

#include "autohdr_effect_kcm.moc"
