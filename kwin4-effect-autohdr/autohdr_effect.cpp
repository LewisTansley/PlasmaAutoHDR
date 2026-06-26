#include "autohdr_effect.h"
#include <effect/effecthandler.h>
#include <effect/effectwindow.h>
#include <opengl/glshadermanager.h>
#include <opengl/glshader.h>
#include <window.h>
#include <QAction>
#include <QDBusConnection>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <KGlobalAccel>
#include <KConfigGroup>
#include <optional>

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

QString locateEffectDataFile(const QString &relativePath)
{
    const QStringList candidates = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, relativePath);
    for (const QString &path : candidates) {
        if (!path.startsWith(QDir::homePath())) {
            return path;
        }
    }
    return candidates.isEmpty() ? QString() : candidates.constFirst();
}

} // namespace

namespace KWin {

    KWIN_EFFECT_FACTORY_SUPPORTED(AutoHDREffect,
                                  "metadata.json",
                                  return KWin::OffscreenEffect::supported();)

    AutoHDREffect::AutoHDREffect()
    {
        m_config = AutoHdr::openConfig();
        loadGlobalDefaults();

        m_toggleAction = new QAction(this);
        m_toggleAction->setAutoRepeat(false);
        m_toggleAction->setObjectName(QStringLiteral("ToggleAutoHDR"));
        registerEffectShortcut(m_toggleAction, QStringLiteral("Toggle AutoHDR for Active Window"),
                               QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_H));

        m_overlayAction = new QAction(this);
        m_overlayAction->setAutoRepeat(false);
        m_overlayAction->setObjectName(QStringLiteral("ToggleAutoHDROverlay"));
        registerEffectShortcut(m_overlayAction, QStringLiteral("Open AutoHDR Calibration Engine"),
                               QKeySequence(Qt::META | Qt::CTRL | Qt::Key_H));

        connect(m_toggleAction, &QAction::triggered, this, &AutoHDREffect::toggleAutoHDR, Qt::QueuedConnection);
        connect(m_overlayAction, &QAction::triggered, this, &AutoHDREffect::toggleOverlay, Qt::QueuedConnection);

        registerDBusService();

        connect(effects, &EffectsHandler::windowAdded, this, &AutoHDREffect::maybeAutoActivateWindow);

        connect(effects, &EffectsHandler::windowClosed, this, [this](EffectWindow *w) {
            m_activeWindows.remove(w);
            m_pendingUnredirects.remove(w);
            if (m_redirectedWindows.contains(w)) {
                m_redirectedWindows.remove(w);
            }
            if (w == m_calibratingWindow) {
                m_calibratingWindow = nullptr;
                m_calibratingAppKey.clear();
            }
        });

        const QString shaderPath = locateEffectDataFile(QStringLiteral("kwin/effects/autohdr/autohdr.frag"));
        if (shaderPath.isEmpty()) {
            qWarning() << "AutoHDR Effect: fragment shader not found";
            return;
        }

        m_shaderPath = shaderPath;
        loadShader();
    }

    AutoHDREffect::~AutoHDREffect()
    {
        unregisterDBusService();
        effects->hideOnScreenMessage();
        if (effects->makeOpenGLContextCurrent()) {
            for (EffectWindow *window : std::as_const(m_redirectedWindows)) {
                unredirect(window);
            }
        }
        m_redirectedWindows.clear();
        m_activeWindows.clear();
        m_pendingUnredirects.clear();
        m_calibratingWindow = nullptr;
    }

    void AutoHDREffect::reconfigure(ReconfigureFlags flags)
    {
        Q_UNUSED(flags)
        m_config->reparseConfiguration();
        loadGlobalDefaults(false);
        reevaluateAllWindows();
    }

    void AutoHDREffect::registerEffectShortcut(QAction *action, const QString &friendlyName,
                                               const QKeySequence &defaultShortcut)
    {
        action->setText(friendlyName);
        action->setProperty("componentName", QStringLiteral("kwin4_effect_autohdr"));
        action->setProperty("componentDisplayName", QStringLiteral("AutoHDR Per-Window Modifier"));

        const QString component = QStringLiteral("kwin4_effect_autohdr");
        const QList<QKeySequence> saved = KGlobalAccel::self()->globalShortcut(component, action->objectName());
        const QList<QKeySequence> defaults{defaultShortcut};
        const QList<QKeySequence> active = saved.isEmpty() ? defaults : saved;

        KGlobalAccel::self()->setDefaultShortcut(action, defaults, KGlobalAccel::Autoloading);
        KGlobalAccel::self()->setShortcut(action, active, KGlobalAccel::Autoloading);
    }

    QString AutoHDREffect::calibrationScriptPath() const
    {
        const QStringList candidates = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
                                                                 QStringLiteral("kwin/effects/autohdr/plasma-autohdr-calibrate"));
        for (const QString &path : candidates) {
            if (QFileInfo::exists(path)) {
                return path;
            }
        }
        return QString();
    }

    QProcessEnvironment AutoHDREffect::calibrationProcessEnvironment() const
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

        if (!env.contains(QStringLiteral("HOME"))) {
            env.insert(QStringLiteral("HOME"), QDir::homePath());
        }

        if (!env.contains(QStringLiteral("WAYLAND_DISPLAY"))) {
            const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
            const QDir runtime(runtimeDir);
            const QStringList sockets = runtime.entryList({QStringLiteral("wayland-*")}, QDir::Files);
            if (!sockets.isEmpty()) {
                env.insert(QStringLiteral("WAYLAND_DISPLAY"), sockets.constFirst());
            }
        }

        if (!env.contains(QStringLiteral("DISPLAY"))) {
            env.insert(QStringLiteral("DISPLAY"), QStringLiteral(":1"));
        }

        const QString runtimeDir =
            env.contains(QStringLiteral("XDG_RUNTIME_DIR"))
                ? env.value(QStringLiteral("XDG_RUNTIME_DIR"))
                : QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (!runtimeDir.isEmpty()) {
            if (!env.contains(QStringLiteral("XDG_RUNTIME_DIR"))) {
                env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtimeDir);
            }
            if (!env.contains(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"))) {
                env.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), runtimeDir + QStringLiteral("/bus"));
            }
        }

        if (!m_calibratingAppKey.isEmpty()) {
            env.insert(QStringLiteral("AUTOHDR_APP_KEY"), m_calibratingAppKey);
            const WindowIdentifiers ids = identifiersForWindow(m_calibratingWindow);
            env.insert(QStringLiteral("AUTOHDR_APP_DISPLAY_NAME"), ids.displayName);
            env.insert(QStringLiteral("AUTOHDR_WINDOW_CLASS"), ids.windowClass);
            env.insert(QStringLiteral("AUTOHDR_RESOURCE_CLASS"), ids.resourceClass);
            env.insert(QStringLiteral("AUTOHDR_DESKTOP_FILE"), ids.desktopFile);
            if (!AutoHdr::hasAppProfile(m_config, m_calibratingAppKey)) {
                env.insert(QStringLiteral("AUTOHDR_APP_IS_NEW"), QStringLiteral("1"));
            }
        }

        return env;
    }

    bool AutoHDREffect::isActive() const
    {
        return !m_activeWindows.isEmpty();
    }

    bool AutoHDREffect::isEligibleWindow(EffectWindow *window) const
    {
        if (!window) {
            return false;
        }
        if (window->isDesktop() || window->isDock() || window->isTooltip() || window->isPopupMenu()
            || window->isDropdownMenu() || window->isMenu() || window->isSplash()) {
            return false;
        }
        return window->isNormalWindow() || window->isDialog() || window->isUtility();
    }

    AutoHDREffect::WindowIdentifiers AutoHDREffect::identifiersForWindow(EffectWindow *window) const
    {
        WindowIdentifiers ids;
        if (!window) {
            return ids;
        }

        ids.windowClass = window->windowClass();
        ids.displayName = window->caption();

        if (Window *coreWindow = window->window()) {
            ids.resourceClass = coreWindow->resourceClass();
            ids.desktopFile = coreWindow->desktopFileName();
            if (!coreWindow->captionNormal().isEmpty()) {
                ids.displayName = coreWindow->captionNormal();
            }
        }

        if (ids.displayName.isEmpty()) {
            ids.displayName = ids.windowClass;
        }

        return ids;
    }

    QString AutoHDREffect::appKeyForWindow(EffectWindow *window) const
    {
        const WindowIdentifiers ids = identifiersForWindow(window);

        if (!ids.desktopFile.isEmpty()) {
            QString desktop = ids.desktopFile;
            if (desktop.endsWith(QStringLiteral(".desktop"))) {
                desktop.chop(8);
            }
            return AutoHdr::sanitizeAppKey(desktop);
        }

        if (!ids.resourceClass.isEmpty()) {
            return AutoHdr::sanitizeAppKey(ids.resourceClass);
        }

        const QStringList parts = ids.windowClass.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QString classPart = parts.isEmpty() ? ids.windowClass : parts.constLast();
        return AutoHdr::sanitizeAppKey(classPart);
    }

    QString AutoHDREffect::findKnownAppKey(EffectWindow *window) const
    {
        const WindowIdentifiers ids = identifiersForWindow(window);
        return AutoHdr::findAppKeyForIdentifiers(m_config, ids.desktopFile, ids.resourceClass, ids.windowClass);
    }

    QString AutoHDREffect::resolvedAppKeyForWindow(EffectWindow *window) const
    {
        if (!window) {
            return QString();
        }

        const QString knownKey = findKnownAppKey(window);
        if (!knownKey.isEmpty()) {
            return knownKey;
        }

        const QString derivedKey = appKeyForWindow(window);
        if (!derivedKey.isEmpty() && AutoHdr::hasAppProfile(m_config, derivedKey)) {
            return derivedKey;
        }

        return QString();
    }

    AutoHDREffect::CalibrationSettings AutoHDREffect::settingsForAppKey(const QString &appKey) const
    {
        if (!appKey.isEmpty()) {
            const std::optional<AutoHdr::AppProfile> profile = AutoHdr::loadAppProfile(m_config, appKey);
            if (profile) {
                CalibrationSettings settings = profile->settings;
                AutoHdr::sanitizeCalibrationSettings(settings, m_hdrReferenceNits, m_hdrMaxDisplayNits, m_config);
                return settings;
            }
        }
        return m_globalDefaults;
    }

    AutoHDREffect::CalibrationSettings AutoHDREffect::settingsForWindow(EffectWindow *window) const
    {
        if (window == m_calibratingWindow && !m_calibratingAppKey.isEmpty()) {
            return settingsForAppKey(m_calibratingAppKey);
        }

        const QString key = resolvedAppKeyForWindow(window);
        if (!key.isEmpty()) {
            return settingsForAppKey(key);
        }

        return m_globalDefaults;
    }

    void AutoHDREffect::reloadHdrDisplayLimits()
    {
        const HdrDisplayLimits limits = readHdrDisplayLimits();
        m_hdrReferenceNits = static_cast<float>(limits.referenceNits);
        m_hdrMaxDisplayNits = static_cast<float>(limits.maxDisplayNits);
    }

    void AutoHDREffect::sanitizeGlobalDefaults(bool persist)
    {
        reloadHdrDisplayLimits();
        AutoHdr::sanitizeCalibrationSettings(m_globalDefaults, m_hdrReferenceNits, m_hdrMaxDisplayNits, m_config);
        if (persist) {
            saveGlobalDefaults();
        }
    }

    void AutoHDREffect::loadGlobalDefaults(bool persistSanitize)
    {
        m_config->reparseConfiguration();
        reloadHdrDisplayLimits();

        m_globalDefaults = AutoHdr::loadGlobalSettings(m_config, m_hdrMaxDisplayNits);
        m_autoActivateCalibrated = AutoHdr::loadGeneralSettings(m_config).autoActivateCalibrated;
        sanitizeGlobalDefaults(persistSanitize);
        loadShader();
    }

    void AutoHDREffect::saveGlobalDefaults()
    {
        AutoHdr::saveGlobalSettings(m_config, m_globalDefaults);
    }

    void AutoHDREffect::reloadActiveWindowSettings()
    {
        QList<EffectWindow *> windows = m_activeWindows.keys();
        for (EffectWindow *window : windows) {
            CalibrationSettings settings;
            if (window == m_calibratingWindow && !m_calibratingAppKey.isEmpty()) {
                settings = settingsForAppKey(m_calibratingAppKey);
            } else {
                const QString key = resolvedAppKeyForWindow(window);
                settings = key.isEmpty() ? m_globalDefaults : settingsForAppKey(key);
            }
            AutoHdr::sanitizeCalibrationSettings(settings, m_hdrReferenceNits, m_hdrMaxDisplayNits, m_config);
            m_activeWindows.insert(window, settings);
        }
    }

    void AutoHDREffect::warnMissingToneCurveUniformsOnce()
    {
        if (m_warnedMissingToneCurveUniforms) {
            return;
        }
        if (m_locToneCurveLut < 0) {
            qWarning() << "AutoHDR Effect: tone curve shader uniform 'toneCurveLut' not found";
        }
        if (m_locToneCurveInputSpan < 0) {
            qWarning() << "AutoHDR Effect: tone curve shader uniform 'toneCurveInputSpan' not found";
        }
        m_warnedMissingToneCurveUniforms = true;
    }

    void AutoHDREffect::computeToneCurveLut(const CalibrationSettings &settings)
    {
        CalibrationSettings sanitized = settings;
        AutoHdr::sanitizeCalibrationSettings(sanitized, m_hdrReferenceNits, m_hdrMaxDisplayNits, m_config);

        const AutoHdr::ToneCurveEndpoints endpoints =
            AutoHdr::toneCurveEndpointsFor(sanitized, m_hdrReferenceNits, m_hdrMaxDisplayNits);
        const QVector<QPointF> fullCurve = AutoHdr::buildFullCurve(endpoints, sanitized.toneCurvePoints);
        const float inputSpan = qMax(endpoints.visualReferenceNits, 1.0f);
        m_cachedToneCurveInputSpan = inputSpan;
        AutoHdr::buildToneCurveLut(fullCurve, inputSpan, m_toneCurveLut, AutoHdr::kToneCurveLutSize);
        m_toneCurveLutDirty = true;
    }

    void AutoHDREffect::uploadToneCurveUniforms()
    {
        if (!m_shader || !m_shader->isValid() || !effects->makeOpenGLContextCurrent()) {
            return;
        }

        warnMissingToneCurveUniformsOnce();

        ShaderBinder binder(m_shader.get());
        if (m_locToneCurveLut >= 0) {
            glUniform1fv(m_locToneCurveLut, AutoHdr::kToneCurveLutSize, m_toneCurveLut);
        }
        if (m_locToneCurveInputSpan >= 0) {
            m_shader->setUniform(m_locToneCurveInputSpan, m_cachedToneCurveInputSpan);
        }
        m_toneCurveLutDirty = false;
    }

    void AutoHDREffect::resolveUniformLocations()
    {
        if (!m_shader || !m_shader->isValid() || !effects->makeOpenGLContextCurrent()) {
            return;
        }

        ShaderBinder binder(m_shader.get());
        m_locGamutExpansion = m_shader->uniformLocation("gamutExpansion");
        m_locBlackPoint = m_shader->uniformLocation("blackPoint");
        m_locColorVibrance = m_shader->uniformLocation("colorVibrance");
        m_locToneCurveInputSpan = m_shader->uniformLocation("toneCurveInputSpan");
        m_locToneCurveLut = m_shader->uniformLocation("toneCurveLut");
        warnMissingToneCurveUniformsOnce();
    }

    void AutoHDREffect::updateUniforms(const CalibrationSettings &settings)
    {
        computeToneCurveLut(settings);

        if (!m_shader || !m_shader->isValid() || !effects->makeOpenGLContextCurrent()) {
            return;
        }

        CalibrationSettings sanitized = settings;
        AutoHdr::sanitizeCalibrationSettings(sanitized, m_hdrReferenceNits, m_hdrMaxDisplayNits, m_config);

        ShaderBinder binder(m_shader.get());
        if (m_locGamutExpansion >= 0) {
            m_shader->setUniform(m_locGamutExpansion, sanitized.gamutExpansion);
        }
        if (m_locBlackPoint >= 0) {
            m_shader->setUniform(m_locBlackPoint, sanitized.blackPoint);
        }
        if (m_locColorVibrance >= 0) {
            m_shader->setUniform(m_locColorVibrance, sanitized.vibrance);
        }

        uploadToneCurveUniforms();
    }

    bool AutoHDREffect::loadShader()
    {
        if (!m_shaderPath.isEmpty()) {
            const QDateTime fragMtime = QFileInfo(m_shaderPath).lastModified();
            if (m_shader && m_shader->isValid()) {
                if (fragMtime.isValid() && fragMtime == m_shaderFragMtime) {
                    return true;
                }
                m_shader.reset();
            }
        } else if (m_shader && m_shader->isValid()) {
            return true;
        }

        if (m_shaderPath.isEmpty()) {
            return false;
        }
        if (!effects->makeOpenGLContextCurrent()) {
            return false;
        }

        QFile file(m_shaderPath);
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "AutoHDR Effect: failed to open fragment shader" << m_shaderPath;
            return false;
        }

        m_shader = ShaderManager::instance()->generateCustomShader(ShaderTrait::MapTexture, QByteArray(),
                                                                   file.readAll());
        if (!m_shader || !m_shader->isValid()) {
            qWarning() << "AutoHDR Effect: failed to compile HDR fragment shader from" << m_shaderPath;
            m_shader.reset();
            return false;
        }

        m_shaderFragMtime = QFileInfo(m_shaderPath).lastModified();
        resolveUniformLocations();
        return true;
    }

    void AutoHDREffect::performUnredirect(EffectWindow *window)
    {
        if (!window || !m_redirectedWindows.contains(window)) {
            m_pendingUnredirects.remove(window);
            return;
        }

        unredirect(window);
        m_redirectedWindows.remove(window);
        m_pendingUnredirects.remove(window);
    }

    void AutoHDREffect::activateWindow(EffectWindow *window, const CalibrationSettings &settings)
    {
        if (!window || !loadShader()) {
            return;
        }

        m_pendingUnredirects.remove(window);

        CalibrationSettings sanitized = settings;
        AutoHdr::sanitizeCalibrationSettings(sanitized, m_hdrReferenceNits, m_hdrMaxDisplayNits, m_config);
        m_activeWindows.insert(window, sanitized);

        updateUniforms(sanitized);

        if (!m_redirectedWindows.contains(window)) {
            redirect(window);
            m_redirectedWindows.insert(window);
        }
        setShader(window, m_shader.get());
        window->addRepaintFull();
    }

    void AutoHDREffect::scheduleUnredirect(EffectWindow *window)
    {
        if (!window) {
            return;
        }

        m_activeWindows.remove(window);

        if (!m_redirectedWindows.contains(window)) {
            m_pendingUnredirects.remove(window);
            return;
        }

        m_pendingUnredirects.insert(window);
        window->addRepaintFull();
        effects->addRepaintFull();
    }

    void AutoHDREffect::maybeAutoActivateWindow(EffectWindow *window)
    {
        if (!m_autoActivateCalibrated || !isEligibleWindow(window)) {
            return;
        }

        const QString knownKey = resolvedAppKeyForWindow(window);
        if (knownKey.isEmpty()) {
            return;
        }

        const std::optional<AutoHdr::AppProfile> profile = AutoHdr::loadAppProfile(m_config, knownKey);
        if (!profile || !profile->metadata.autoActivate) {
            return;
        }

        if (m_activeWindows.contains(window)) {
            return;
        }

        activateWindow(window, profile->settings);
        qInfo() << "AutoHDR Effect: auto-activated for" << window->windowClass();
    }

    void AutoHDREffect::reevaluateAllWindows()
    {
        QList<EffectWindow *> toDeactivate;
        const QList<EffectWindow *> activeCopy = m_activeWindows.keys();
        for (EffectWindow *window : activeCopy) {
            const QString knownKey = resolvedAppKeyForWindow(window);
            if (knownKey.isEmpty()) {
                activateWindow(window, m_globalDefaults);
                continue;
            }

            const std::optional<AutoHdr::AppProfile> profile = AutoHdr::loadAppProfile(m_config, knownKey);
            if (!profile) {
                toDeactivate.append(window);
                continue;
            }

            if (!profile->metadata.autoActivate) {
                toDeactivate.append(window);
                continue;
            }

            activateWindow(window, profile->settings);
        }

        for (EffectWindow *window : toDeactivate) {
            scheduleUnredirect(window);
        }

        for (EffectWindow *window : effects->stackingOrder()) {
            maybeAutoActivateWindow(window);
        }
    }

    void AutoHDREffect::prePaintWindow(RenderView *view, EffectWindow *w, WindowPrePaintData &data,
                                       std::chrono::milliseconds presentTime)
    {
        if (m_pendingUnredirects.contains(w) && !m_activeWindows.contains(w)) {
            performUnredirect(w);
        }

        if (m_activeWindows.contains(w) && m_shader && m_shader->isValid()) {
            updateUniforms(m_activeWindows.value(w));
            if (m_toneCurveLutDirty) {
                uploadToneCurveUniforms();
            }
        }

        effects->prePaintWindow(view, w, data, presentTime);
    }

    void AutoHDREffect::drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                   EffectWindow *window, int mask, const Region &deviceRegion, WindowPaintData &data)
    {
        if (m_activeWindows.contains(window) && m_shader && m_shader->isValid()) {
            updateUniforms(m_activeWindows.value(window));
            if (m_toneCurveLutDirty) {
                uploadToneCurveUniforms();
            }
        }
        OffscreenEffect::drawWindow(renderTarget, viewport, window, mask, deviceRegion, data);
    }

    void AutoHDREffect::toggleAutoHDR()
    {
        if (QGuiApplication::queryKeyboardModifiers() & Qt::ControlModifier) {
            QMetaObject::invokeMethod(this, &AutoHDREffect::toggleOverlay, Qt::QueuedConnection);
            return;
        }

        EffectWindow *active = effects->activeWindow();
        if (!active) {
            qWarning() << "AutoHDR Effect: no active window to target";
            return;
        }

        if (!loadShader()) {
            qWarning() << "AutoHDR Effect: shader is not available";
            return;
        }

        if (m_activeWindows.contains(active)) {
            scheduleUnredirect(active);
            qInfo() << "AutoHDR Effect: disabled for" << active->windowClass();
            return;
        }

        if (m_pendingUnredirects.contains(active)) {
            m_pendingUnredirects.remove(active);
            const QString knownKey = resolvedAppKeyForWindow(active);
            const CalibrationSettings settings = knownKey.isEmpty() ? m_globalDefaults : settingsForAppKey(knownKey);
            activateWindow(active, settings);
            qInfo() << "AutoHDR Effect: re-enabled for" << active->windowClass();
            return;
        }

        const QString knownKey = resolvedAppKeyForWindow(active);
        const CalibrationSettings settings = knownKey.isEmpty() ? m_globalDefaults : settingsForAppKey(knownKey);
        activateWindow(active, settings);
        qInfo() << "AutoHDR Effect: enabled for" << active->windowClass();
    }

    void AutoHDREffect::showTransientOnScreenMessage(const QString &message, const QString &iconName)
    {
        effects->showOnScreenMessage(message, iconName);
        QTimer::singleShot(3000, this, [this]() {
            effects->hideOnScreenMessage();
        });
    }

    void AutoHDREffect::registerDBusService()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) {
            qWarning() << "AutoHDR Effect: session D-Bus unavailable";
            return;
        }

        if (!bus.registerService(QStringLiteral("org.kde.kwin.effect.autohdr"))) {
            qWarning() << "AutoHDR Effect: failed to register D-Bus service";
            return;
        }

        if (!bus.registerObject(QStringLiteral("/autohdr"), this,
                                QDBusConnection::ExportAllSlots | QDBusConnection::ExportScriptableSlots)) {
            qWarning() << "AutoHDR Effect: failed to register D-Bus object";
            bus.unregisterService(QStringLiteral("org.kde.kwin.effect.autohdr"));
            return;
        }

        m_dbusRegistered = true;
    }

    void AutoHDREffect::unregisterDBusService()
    {
        if (!m_dbusRegistered) {
            return;
        }

        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.unregisterObject(QStringLiteral("/autohdr"));
        bus.unregisterService(QStringLiteral("org.kde.kwin.effect.autohdr"));
        m_dbusRegistered = false;
    }

    void AutoHDREffect::reloadSettings()
    {
        if (QThread::currentThread() != thread()) {
            QMetaObject::invokeMethod(this, "reloadSettings", Qt::BlockingQueuedConnection);
            return;
        }

        m_config->reparseConfiguration();
        loadGlobalDefaults(false);
        reloadActiveWindowSettings();
        repaintActiveWindows();
    }

    void AutoHDREffect::repaintActiveWindows()
    {
        for (auto it = m_activeWindows.constBegin(); it != m_activeWindows.constEnd(); ++it) {
            activateWindow(it.key(), it.value());
        }
        if (!m_activeWindows.isEmpty()) {
            effects->addRepaintFull();
        }
    }

    void AutoHDREffect::finishCalibration(bool saved)
    {
        m_kdialogProcess = nullptr;
        m_calibratingWindow = nullptr;
        m_calibratingAppKey.clear();

        reloadSettings();

        if (saved) {
            showTransientOnScreenMessage(QStringLiteral("AutoHDR calibration confirmed"),
                                         QStringLiteral("video-display"));
        }
    }

    void AutoHDREffect::runCalibrationDialog()
    {
        const QString scriptPath = calibrationScriptPath();
        if (scriptPath.isEmpty()) {
            qWarning() << "AutoHDR Effect: calibration script not found";
            showTransientOnScreenMessage(QStringLiteral("AutoHDR calibration script not found"),
                                         QStringLiteral("dialog-error"));
            return;
        }

        auto *process = new QProcess(this);
        m_kdialogProcess = process;

        connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
            qWarning() << "AutoHDR Effect: calibration script error:" << error << process->errorString();
            if (m_kdialogProcess == process) {
                finishCalibration(false);
                showTransientOnScreenMessage(QStringLiteral("AutoHDR calibration dialog failed to open"),
                                             QStringLiteral("dialog-error"));
            }
            process->deleteLater();
        });

        connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus) {
            if (m_kdialogProcess == process) {
                m_kdialogProcess = nullptr;
            }

            process->deleteLater();
            effects->hideOnScreenMessage();

            if (exitCode != 0) {
                qInfo() << "AutoHDR Effect: calibration dialog cancelled";
                finishCalibration(false);
                return;
            }

            finishCalibration(true);
        });

        qInfo() << "AutoHDR Effect: launching calibration script" << scriptPath;
        process->setProcessEnvironment(calibrationProcessEnvironment());
        process->start(scriptPath, QStringList());
    }

    void AutoHDREffect::toggleOverlay()
    {
        if (m_kdialogProcess && m_kdialogProcess->state() != QProcess::NotRunning) {
            qInfo() << "AutoHDR Effect: calibration dialog already open";
            return;
        }
        m_kdialogProcess = nullptr;

        EffectWindow *active = effects->activeWindow();
        if (!active) {
            showTransientOnScreenMessage(QStringLiteral("Select a window to calibrate AutoHDR"),
                                         QStringLiteral("dialog-warning"));
            return;
        }

        m_calibratingWindow = active;
        m_calibratingAppKey = appKeyForWindow(active);

        if (!m_activeWindows.contains(active)) {
            const CalibrationSettings settings = settingsForWindow(active);
            activateWindow(active, settings);
        }

        qInfo() << "AutoHDR Effect: opening calibration for" << active->windowClass()
                << "app key" << m_calibratingAppKey;
        showTransientOnScreenMessage(QStringLiteral("Opening AutoHDR calibration…"), QStringLiteral("configure"));
        QMetaObject::invokeMethod(this, &AutoHDREffect::runCalibrationDialog, Qt::QueuedConnection);
    }

} // namespace KWin

#include "autohdr_effect.moc"
