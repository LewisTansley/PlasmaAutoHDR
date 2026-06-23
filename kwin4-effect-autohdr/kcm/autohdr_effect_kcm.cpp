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
#include <QSpinBox>
#include <QTabWidget>
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

        m_modeTabs = new QTabWidget(defaultsGroup);
        auto *simplePage = new QWidget(defaultsGroup);
        auto *simpleLayout = new QFormLayout(simplePage);

        m_maxNits = new QSpinBox(simplePage);
        m_maxNits->setRange(1, 10000);
        m_maxNits->setSingleStep(10);
        simpleLayout->addRow(i18n("Peak brightness (nits):"), m_maxNits);

        m_midPoint = new QSpinBox(simplePage);
        m_midPoint->setRange(80, 480);
        simpleLayout->addRow(i18n("Paper white (nits):"), m_midPoint);

        m_blackPoint = new QDoubleSpinBox(simplePage);
        m_blackPoint->setRange(-0.01, 0.01);
        m_blackPoint->setDecimals(4);
        m_blackPoint->setSingleStep(0.0001);
        simpleLayout->addRow(i18n("Black point:"), m_blackPoint);

        m_highlightExpansion = new QDoubleSpinBox(simplePage);
        m_highlightExpansion->setRange(AutoHdr::kHighlightExpansionMin, AutoHdr::kHighlightExpansionMax);
        m_highlightExpansion->setSingleStep(0.1);
        simpleLayout->addRow(i18n("Highlight expansion:"), m_highlightExpansion);

        m_highlightLift = new QDoubleSpinBox(simplePage);
        m_highlightLift->setRange(AutoHdr::kHighlightLiftMin, AutoHdr::kHighlightLiftMax);
        m_highlightLift->setSingleStep(0.1);
        simpleLayout->addRow(i18n("Highlight lift:"), m_highlightLift);

        m_highlightRange = new QDoubleSpinBox(simplePage);
        m_highlightRange->setRange(0.0, AutoHdr::kHighlightRangeMax);
        m_highlightRange->setSingleStep(0.1);
        simpleLayout->addRow(i18n("Highlight range:"), m_highlightRange);

        m_modeTabs->addTab(simplePage, i18n("Simple"));

        m_toneCurveEditor = new ToneCurveEditor(defaultsGroup);
        m_modeTabs->addTab(m_toneCurveEditor, i18n("Tone Curve"));

        defaultsOuterLayout->addWidget(m_modeTabs);

        auto *sharedLayout = new QFormLayout();
        m_vibrance = new QDoubleSpinBox(defaultsGroup);
        m_vibrance->setRange(0.0, 10.0);
        m_vibrance->setSingleStep(0.1);
        sharedLayout->addRow(i18n("Vibrance:"), m_vibrance);

        m_gamutExpansion = new QDoubleSpinBox(defaultsGroup);
        m_gamutExpansion->setRange(0.0, 20.0);
        m_gamutExpansion->setSingleStep(0.1);
        sharedLayout->addRow(i18n("Gamut expansion:"), m_gamutExpansion);
        defaultsOuterLayout->addLayout(sharedLayout);

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

        connect(m_maxNits, qOverload<int>(&QSpinBox::valueChanged), this, &KCModule::markAsChanged);
        connect(m_midPoint, qOverload<int>(&QSpinBox::valueChanged), this, &KCModule::markAsChanged);
        connect(m_blackPoint, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &KCModule::markAsChanged);
        connect(m_vibrance, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &KCModule::markAsChanged);
        connect(m_highlightExpansion, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &KCModule::markAsChanged);
        connect(m_highlightLift, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &KCModule::markAsChanged);
        connect(m_highlightRange, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &KCModule::markAsChanged);
        connect(m_gamutExpansion, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &KCModule::markAsChanged);
        connect(m_autoActivate, &QCheckBox::toggled, this, &KCModule::markAsChanged);
        connect(m_modeTabs, &QTabWidget::currentChanged, this, [this](int index) {
            if (index == 1 && !m_curveSeeded) {
                m_toneCurveEditor->seedFromLegacy(static_cast<float>(m_midPoint->value()),
                                                  static_cast<float>(m_maxNits->value()));
                m_curveSeeded = true;
            }
            markAsChanged();
        });
        connect(m_toneCurveEditor, &ToneCurveEditor::settingsChanged, this, &KCModule::markAsChanged);
    }

    void load() override
    {
        KCModule::load();

        const HdrDisplayLimits limits = readHdrDisplayLimits();
        m_maxNits->setRange(limits.referenceNits, limits.maxDisplayNits);
        m_toneCurveEditor->setHdrLimits(limits.referenceNits + 1, limits.maxDisplayNits);

        m_config->reparseConfiguration();
        const AutoHdr::CalibrationSettings globals =
            AutoHdr::loadGlobalSettings(m_config, static_cast<float>(limits.maxDisplayNits));

        m_maxNits->setValue(qRound(globals.maxNits));
        m_midPoint->setValue(qRound(globals.midPoint));
        m_blackPoint->setValue(globals.blackPoint);
        m_vibrance->setValue(globals.vibrance);
        m_highlightExpansion->setValue(globals.highlightExpansion);
        m_highlightLift->setValue(globals.highlightLift);
        m_highlightRange->setValue(globals.highlightRange);
        m_gamutExpansion->setValue(globals.gamutExpansion);
        m_toneCurveEditor->setValues(globals.maxNits, globals.referenceNits, globals.sdrMaxPoint,
                                     globals.toneCurvePoints);
        m_modeTabs->setCurrentIndex(globals.useToneCurve ? 1 : 0);
        m_curveSeeded = globals.useToneCurve;

        m_autoActivate->setChecked(AutoHdr::loadGeneralSettings(m_config).autoActivateCalibrated);

        rebuildAppsTable();
    }

    void save() override
    {
        const HdrDisplayLimits limits = readHdrDisplayLimits();

        AutoHdr::CalibrationSettings globals =
            AutoHdr::loadGlobalSettings(m_config, static_cast<float>(limits.maxDisplayNits));
        globals.maxNits = static_cast<float>(m_maxNits->value());
        globals.midPoint = static_cast<float>(m_midPoint->value());
        globals.blackPoint = static_cast<float>(m_blackPoint->value());
        globals.vibrance = static_cast<float>(m_vibrance->value());
        globals.highlightExpansion = static_cast<float>(m_highlightExpansion->value());
        globals.highlightLift = static_cast<float>(m_highlightLift->value());
        globals.highlightRange = static_cast<float>(m_highlightRange->value());
        globals.gamutExpansion = static_cast<float>(m_gamutExpansion->value());
        globals.useToneCurve = m_modeTabs->currentIndex() == 1;

        float peakNits = 0.0f;
        float referenceNits = 0.0f;
        QPointF sdrMaxPoint;
        QVector<QPointF> intermediatePoints;
        m_toneCurveEditor->getValues(peakNits, referenceNits, sdrMaxPoint, intermediatePoints);
        if (globals.useToneCurve) {
            globals.maxNits = peakNits;
            globals.referenceNits = referenceNits;
            globals.sdrMaxPoint = sdrMaxPoint;
            globals.toneCurvePoints = intermediatePoints;
        }

        AutoHdr::sanitizeCalibrationSettings(globals, static_cast<float>(limits.referenceNits),
                                             static_cast<float>(limits.maxDisplayNits));
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
    QSpinBox *m_maxNits = nullptr;
    QSpinBox *m_midPoint = nullptr;
    QDoubleSpinBox *m_blackPoint = nullptr;
    QDoubleSpinBox *m_vibrance = nullptr;
    QDoubleSpinBox *m_highlightExpansion = nullptr;
    QDoubleSpinBox *m_highlightLift = nullptr;
    QDoubleSpinBox *m_highlightRange = nullptr;
    QDoubleSpinBox *m_gamutExpansion = nullptr;
    QCheckBox *m_autoActivate = nullptr;
    QTabWidget *m_modeTabs = nullptr;
    ToneCurveEditor *m_toneCurveEditor = nullptr;
    bool m_curveSeeded = false;
    QTableWidget *m_appsTable = nullptr;
    QStringList m_appKeys;
    QSet<QString> m_pendingDeletes;
};

K_PLUGIN_CLASS_WITH_JSON(AutoHdrEffectKcm, "kwin4_effect_autohdr_config.json")

#include "autohdr_effect_kcm.moc"
