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
        connect(m_toneCurveEditor, &ToneCurveEditor::settingsChanged, this, &KCModule::markAsChanged);
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
                                     globals.chromaCompensation);

        m_autoActivate->setChecked(AutoHdr::loadGeneralSettings(m_config).autoActivateCalibrated);

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
        QPointF sdrMaxPoint;
        QVector<QPointF> intermediatePoints;
        AutoHdr::ToneCurvePreset toneCurvePreset = AutoHdr::ToneCurvePreset::Linear;
        QString toneCurveUserPresetId;
        m_toneCurveEditor->getValues(peakNits, referenceNits, sdrMaxPoint, intermediatePoints, blackPoint,
                                     toneCurvePreset, toneCurveUserPresetId, vibrance, gamutExpansion,
                                     chromaCompensation);
        globals.vibrance = vibrance;
        globals.gamutExpansion = gamutExpansion;
        globals.chromaCompensation = chromaCompensation;
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
    ToneCurveEditor *m_toneCurveEditor = nullptr;
    QTableWidget *m_appsTable = nullptr;
    QStringList m_appKeys;
    QSet<QString> m_pendingDeletes;
};

K_PLUGIN_CLASS_WITH_JSON(AutoHdrEffectKcm, "kwin4_effect_autohdr_config.json")

#include "autohdr_effect_kcm.moc"
