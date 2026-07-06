#include "autohdr_effect.h"
#include "autohdr_display.h"
#include <algorithm>
#include <cmath>
#include <core/backendoutput.h>
#include <core/colorspace.h>
#include <core/region.h>
#include <core/pixelgrid.h>
#include <core/rendertarget.h>
#include <core/renderviewport.h>
#include <effect/effecthandler.h>
#include <effect/effectwindow.h>
#include <opengl/eglcontext.h>
#include <opengl/glframebuffer.h>
#include <opengl/glshadermanager.h>
#include <opengl/glshader.h>
#include <opengl/gltexture.h>
#include <opengl/glutils.h>
#include <opengl/glvertexbuffer.h>
#include <scene/item.h>
#include <scene/itemgeometry.h>
#include <scene/surfaceitem.h>
#include <scene/windowitem.h>
#include <scene/scene.h>
#include <wayland/surface.h>
#include <window.h>
#include <workspace.h>
#include <QAction>
#include <QDBusConnection>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

bool debugPaintEnabled()
{
    static const bool enabled = qEnvironmentVariableIsSet("AUTOHDR_DEBUG_PAINT");
    return enabled;
}

} // namespace

namespace KWin {

    KWIN_EFFECT_FACTORY_SUPPORTED(AutoHDREffect,
                                  "metadata.json",
                                  return effects && effects->isOpenGLCompositing();)

    AutoHDREffect::AutoHDREffect()
    {
        if (qEnvironmentVariableIntValue("AUTOHDR_SPATIAL_AVG") == 0) {
            m_processingQuality = 0;
        }
        // qEnvironmentVariableIntValue returns 0 when unset — only honor an explicit value.
        if (qEnvironmentVariableIsSet("AUTOHDR_AI")) {
            m_aiForceDisabled = qEnvironmentVariableIntValue("AUTOHDR_AI") == 0;
        }

        if (qEnvironmentVariableIsSet("AUTOHDR_CURVE_AA")) {
            bool ok = false;
            const float value = qEnvironmentVariable("AUTOHDR_CURVE_AA").toFloat(&ok);
            if (ok) {
                m_curveAntialiasStrength = AutoHdr::clampCurveAntialiasStrength(value);
            }
        }
        if (qEnvironmentVariableIsSet("AUTOHDR_HIGHLIGHT_SOFTNESS")) {
            bool ok = false;
            const float value = qEnvironmentVariable("AUTOHDR_HIGHLIGHT_SOFTNESS").toFloat(&ok);
            if (ok) {
                m_highlightSoftness = AutoHdr::clampHighlightSoftness(value);
            }
        }
        if (qEnvironmentVariableIsSet("AUTOHDR_AA_QUALITY")) {
            m_antiAliasingQuality = AutoHdr::clampAntiAliasingQuality(qEnvironmentVariableIntValue("AUTOHDR_AA_QUALITY"));
        }

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
            disconnectWindowOutputTracking(w);
            removeStatusToast(w);
            if (w == m_calibratingWindow) {
                closeCalibrationOverlay(false);
            }
            ensureCompositorHeartbeat();
        });

        m_compositorHeartbeat.setInterval(16);
        m_compositorHeartbeat.setTimerType(Qt::PreciseTimer);
        connect(&m_compositorHeartbeat, &QTimer::timeout, this, [this]() {
            onCompositorHeartbeat();
        });

        connectOutputTracking();

        const QString shaderPath = AutoHdr::locateEffectDataFile(QStringLiteral("kwin/effects/autohdr/autohdr.frag"));
        if (shaderPath.isEmpty()) {
            qWarning() << "AutoHDR Effect: fragment shader not found";
            return;
        }

        m_shaderPath = shaderPath;
        loadShader();
        loadPassShaders();
        initGuidanceInference();
        if (effects->makeOpenGLContextCurrent()) {
            captureInternalFormat();
        }
    }

    AutoHDREffect::~AutoHDREffect()
    {
        m_compositorHeartbeat.stop();
        if (m_guidanceWorker) {
            m_guidanceWorker->shutdown();
            m_guidanceWorker.reset();
        }
        unregisterDBusService();
        effects->hideOnScreenMessage();
        if (m_calibrationOverlay) {
            m_calibrationOverlay->hide();
            delete m_calibrationOverlay;
            m_calibrationOverlay = nullptr;
        }
        const QList<EffectWindow *> toastWindows = m_statusToasts.keys();
        for (EffectWindow *window : toastWindows) {
            removeStatusToast(window);
        }
        if (effects->makeOpenGLContextCurrent()) {
            if (m_ortPbo[0] != 0) {
                glDeleteBuffers(2, m_ortPbo);
                m_ortPbo[0] = 0;
                m_ortPbo[1] = 0;
            }
            QList<EffectWindow *> redirected;
            for (const auto &entry : m_offscreenWindows) {
                redirected.append(entry.first);
            }
            for (EffectWindow *window : redirected) {
                unredirect(window);
            }
        }
        m_offscreenWindows.clear();
        m_activeWindows.clear();
        m_pendingUnredirects.clear();
        disconnectOutputTracking();
        destroyOffscreenConnections();
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
        if (window == m_calibratingWindow && m_calibrationDraftActive) {
            return m_calibrationDraft;
        }

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
        onOutputConfigurationChanged();
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
        const AutoHdr::GeneralSettings general = AutoHdr::loadGeneralSettings(m_config);
        m_autoActivateCalibrated = general.autoActivateCalibrated;
        m_perceptualColorEnabled = general.perceptualColorEnabled;
        if (qEnvironmentVariableIsSet("AUTOHDR_PERCEPTUAL")) {
            m_perceptualColorEnabled = qEnvironmentVariableIntValue("AUTOHDR_PERCEPTUAL") != 0;
        }
        if (!qEnvironmentVariableIsSet("AUTOHDR_CURVE_AA")) {
            m_curveAntialiasStrength = general.curveAntialiasStrength;
        }
        if (!qEnvironmentVariableIsSet("AUTOHDR_HIGHLIGHT_SOFTNESS")) {
            m_highlightSoftness = general.highlightSoftness;
        }
        if (!qEnvironmentVariableIsSet("AUTOHDR_AA_QUALITY")) {
            m_antiAliasingQuality = general.antiAliasingQuality;
        }
        m_aiEnabledGlobal = general.aiEnhanced;
        m_aiStrengthGlobal = general.aiStrength;
        m_aiBandingStrengthGlobal = general.aiBandingStrength;
        m_aiQuality = general.aiQuality;
        m_aiBackend = general.aiBackend;
        m_aiGuidanceModel = general.aiGuidanceModel;
        m_globalDefaults.aiEnhanced = general.aiEnhanced;
        m_globalDefaults.aiStrength = general.aiStrength;
        if (qEnvironmentVariableIsSet("AUTOHDR_AI_QUALITY")) {
            m_aiQuality = AutoHdr::aiQualityFromString(qEnvironmentVariable("AUTOHDR_AI_QUALITY"));
        }
        // qEnvironmentVariableIntValue returns 0 when unset — only honor an explicit value.
        if (qEnvironmentVariableIsSet("AUTOHDR_AI")) {
            if (qEnvironmentVariableIntValue("AUTOHDR_AI") == 0) {
                m_aiForceDisabled = true;
            } else {
                m_aiForceDisabled = false;
                m_aiEnabledGlobal = true;
            }
        }
        sanitizeGlobalDefaults(persistSanitize);
        loadShader();
        loadPassShaders();
        initGuidanceInference();
        invalidateAllGuidance();
        m_loggedGuidanceBackend = false;
        m_loggedAiResourcesFailure = false;
        m_redirectInternalFormat = 0;
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
            if (window == m_calibratingWindow && m_calibrationDraftActive) {
                settings = m_calibrationDraft;
            } else if (window == m_calibratingWindow && !m_calibratingAppKey.isEmpty()) {
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

    void AutoHDREffect::warnMissingPerceptualUniformsOnce()
    {
        if (m_warnedMissingPerceptualUniforms) {
            return;
        }
        if (m_locPerceptualColorEnabled < 0) {
            qWarning() << "AutoHDR Effect: shader uniform 'perceptualColorEnabled' not found";
        }
        m_warnedMissingPerceptualUniforms = true;
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
        AutoHdr::buildToneCurveSlopeLut(m_toneCurveLut, m_toneCurveSlopeLut, AutoHdr::kToneCurveLutSize,
                                        &m_toneCurveMaxSlope);
        m_toneCurveLutDirty = true;
    }

    void AutoHDREffect::uploadToneCurveUniforms()
    {
        if (!m_shader || !effects->makeOpenGLContextCurrent()) {
            return;
        }

        warnMissingToneCurveUniformsOnce();

        ShaderBinder binder(m_shader.get());
        if (m_locToneCurveLut >= 0) {
            glUniform1fv(m_locToneCurveLut, AutoHdr::kToneCurveLutSize, m_toneCurveLut);
        }
        if (m_locToneCurveSlopeLut >= 0) {
            glUniform1fv(m_locToneCurveSlopeLut, AutoHdr::kToneCurveLutSize, m_toneCurveSlopeLut);
        }
        if (m_locToneCurveMaxSlope >= 0) {
            m_shader->setUniform(m_locToneCurveMaxSlope, qMax(m_toneCurveMaxSlope, 1e-6f));
        }
        if (m_locToneCurveInputSpan >= 0) {
            m_shader->setUniform(m_locToneCurveInputSpan, m_cachedToneCurveInputSpan);
        }
        m_toneCurveLutDirty = false;
    }

    void AutoHDREffect::resolveUniformLocations()
    {
        if (!m_shader || !effects->makeOpenGLContextCurrent()) {
            return;
        }

        ShaderBinder binder(m_shader.get());
        m_locGamutExpansion = m_shader->uniformLocation("gamutExpansion");
        m_locBlackPoint = m_shader->uniformLocation("blackPoint");
        m_locColorIntensity = m_shader->uniformLocation("colorIntensity");
        m_locPqBoostParams = m_shader->uniformLocation("pqBoostParams");
        m_locPerceptualColorEnabled = m_shader->uniformLocation("perceptualColorEnabled");
        m_locToneCurveInputSpan = m_shader->uniformLocation("toneCurveInputSpan");
        m_locToneCurveLut = m_shader->uniformLocation("toneCurveLut");
        m_locDebandStrength = m_shader->uniformLocation("debandStrength");
        m_locDitherStrength = m_shader->uniformLocation("ditherStrength");
        m_locCurveAntialiasStrength = m_shader->uniformLocation("curveAntialiasStrength");
        m_locHighlightSoftness = m_shader->uniformLocation("highlightSoftness");
        m_locToneCurveSlopeLut = m_shader->uniformLocation("toneCurveSlopeLut");
        m_locToneCurveMaxSlope = m_shader->uniformLocation("toneCurveMaxSlope");
        m_locProcessingQuality = m_shader->uniformLocation("processingQuality");
        m_locAntiAliasingQuality = m_shader->uniformLocation("antiAliasingQuality");
        m_locEnableSpatialAvgPreCurve = m_shader->uniformLocation("enableSpatialAvgPreCurve");
        m_locGuidanceMap = m_shader->uniformLocation("guidanceMap");
        m_locAiStrength = m_shader->uniformLocation("aiStrength");
        m_locAiBandingStrength = m_shader->uniformLocation("aiBandingStrength");
        m_locAiEnhanced = m_shader->uniformLocation("aiEnhanced");
        warnMissingToneCurveUniformsOnce();
        warnMissingPerceptualUniformsOnce();
        if (!m_warnedMissingAiUniforms && (m_locAiEnhanced < 0 || m_locAiStrength < 0)) {
            m_warnedMissingAiUniforms = true;
            qWarning() << "AutoHDR Effect: AI uniforms missing (aiEnhanced=" << m_locAiEnhanced
                       << "aiStrength=" << m_locAiStrength << ") — AI path will be a no-op";
        }
    }

    void AutoHDREffect::updateUniforms(const CalibrationSettings &settings)
    {
        computeToneCurveLut(settings);

        if (!m_shader || !effects->makeOpenGLContextCurrent()) {
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
        if (m_locColorIntensity >= 0) {
            m_shader->setUniform(m_locColorIntensity, sanitized.colorIntensity);
        }
        if (m_locPqBoostParams >= 0) {
            const QVector4D pqParams =
                AutoHdr::computePqBoostParams(sanitized, m_hdrReferenceNits, m_hdrMaxDisplayNits);
            m_shader->setUniform(m_locPqBoostParams, pqParams);
        }
        if (m_locPerceptualColorEnabled >= 0) {
            m_shader->setUniform(m_locPerceptualColorEnabled, m_perceptualColorEnabled ? 1 : 0);
        }
        if (m_locDebandStrength >= 0) {
            m_shader->setUniform(m_locDebandStrength, m_debandStrength);
        }
        if (m_locDitherStrength >= 0) {
            m_shader->setUniform(m_locDitherStrength, m_ditherStrength);
        }
        if (m_locCurveAntialiasStrength >= 0) {
            m_shader->setUniform(m_locCurveAntialiasStrength, m_curveAntialiasStrength);
        }
        if (m_locHighlightSoftness >= 0) {
            m_shader->setUniform(m_locHighlightSoftness, m_highlightSoftness);
        }
        if (m_locProcessingQuality >= 0) {
            m_shader->setUniform(m_locProcessingQuality, m_processingQuality);
        }
        if (m_locAntiAliasingQuality >= 0) {
            m_shader->setUniform(m_locAntiAliasingQuality, m_antiAliasingQuality);
        }
        if (m_locEnableSpatialAvgPreCurve >= 0) {
            const int enablePreCurve =
                (captureInternalFormat() == GL_RGBA8 || m_curveAntialiasStrength > 0.0f) ? 1 : 0;
            m_shader->setUniform(m_locEnableSpatialAvgPreCurve, enablePreCurve);
        }
        if (m_locAiStrength >= 0) {
            m_shader->setUniform(m_locAiStrength,
                                 shouldUseAi(settings) ? AutoHdr::clampAiStrength(settings.aiStrength) : 0.0f);
        }
        if (m_locAiEnhanced >= 0) {
            m_shader->setUniform(m_locAiEnhanced, shouldUseAi(settings) ? 1 : 0);
        }
        if (m_locAiBandingStrength >= 0) {
            m_shader->setUniform(m_locAiBandingStrength, AutoHdr::clampAiBandingStrength(m_aiBandingStrengthGlobal));
        }
        if (m_locGuidanceMap >= 0) {
            m_shader->setUniform(m_locGuidanceMap, 1);
        }

        uploadToneCurveUniforms();
    }

    QByteArray AutoHDREffect::loadShaderSource() const
    {
        QFile fragFile(m_shaderPath);
        if (!fragFile.open(QIODevice::ReadOnly)) {
            return {};
        }

        QByteArray source = fragFile.readAll();
        const QString shaderDir = QFileInfo(m_shaderPath).absolutePath();

        const auto loadInclude = [&](const char *filename) -> QByteArray {
            QFile includeFile(shaderDir + QLatin1Char('/') + QLatin1String(filename));
            if (!includeFile.open(QIODevice::ReadOnly)) {
                qWarning() << "AutoHDR Effect: shader module not found at" << includeFile.fileName();
                return {};
            }
            return includeFile.readAll();
        };

        const QByteArray colorSource = loadInclude("autohdr_color.glsl");
        const QByteArray perceptualSource = loadInclude("autohdr_perceptual.glsl");
        if (colorSource.isEmpty() || perceptualSource.isEmpty()) {
            return {};
        }

        const auto replaceInclude = [&](const char *filename, const QByteArray &includeSource) {
            const QByteArray includeDirective =
                QByteArrayLiteral("#include \"") + filename + QByteArrayLiteral("\"");
            const int includePos = source.indexOf(includeDirective);
            if (includePos >= 0) {
                source.replace(includePos, includeDirective.size(), includeSource);
            }
        };

        replaceInclude("autohdr_color.glsl", colorSource);
        replaceInclude("autohdr_perceptual.glsl", perceptualSource);
        return source;
    }

    bool AutoHDREffect::loadShader()
    {
        if (!m_shaderPath.isEmpty()) {
            const QDateTime fragMtime = QFileInfo(m_shaderPath).lastModified();
            const QString shaderDir = QFileInfo(m_shaderPath).absolutePath();
            const QDateTime colorMtime =
                QFileInfo(shaderDir + QStringLiteral("/autohdr_color.glsl")).lastModified();
            const QDateTime perceptualMtime =
                QFileInfo(shaderDir + QStringLiteral("/autohdr_perceptual.glsl")).lastModified();
            if (m_shader) {
                if (fragMtime.isValid() && fragMtime == m_shaderFragMtime && colorMtime == m_shaderColorMtime
                    && perceptualMtime == m_shaderPerceptualMtime) {
                    return true;
                }
                m_shader.reset();
            }
        } else if (m_shader) {
            return true;
        }

        if (m_shaderPath.isEmpty()) {
            return false;
        }
        if (!effects->makeOpenGLContextCurrent()) {
            return false;
        }

        const QByteArray source = loadShaderSource();
        if (source.isEmpty()) {
            qWarning() << "AutoHDR Effect: failed to open fragment shader" << m_shaderPath;
            return false;
        }

        m_shader = ShaderManager::instance()->generateCustomShader(ShaderTrait::MapTexture, QByteArray(), source);
        if (!m_shader) {
            qWarning() << "AutoHDR Effect: failed to compile HDR fragment shader from" << m_shaderPath;
            m_shader.reset();
            return false;
        }

        m_shaderFragMtime = QFileInfo(m_shaderPath).lastModified();
        const QString shaderDir = QFileInfo(m_shaderPath).absolutePath();
        m_shaderColorMtime = QFileInfo(shaderDir + QStringLiteral("/autohdr_color.glsl")).lastModified();
        m_shaderPerceptualMtime =
            QFileInfo(shaderDir + QStringLiteral("/autohdr_perceptual.glsl")).lastModified();
        resolveUniformLocations();
        return true;
    }

    GLenum AutoHDREffect::redirectInternalFormat() const
    {
        return captureInternalFormat();
    }

    GLenum AutoHDREffect::captureInternalFormat() const
    {
        if (m_redirectInternalFormat != 0) {
            return m_redirectInternalFormat;
        }

        const bool wantFloat =
            qEnvironmentVariableIsSet("AUTOHDR_FLOAT_FBO")
            || (m_aiEnabledGlobal && !m_aiForceDisabled)
            || (m_perceptualColorEnabled && !m_aiForceDisabled);
        m_redirectInternalFormat = GL_RGBA8;
        if (wantFloat) {
            if (auto probe = GLTexture::allocate(GL_RGBA16F, QSize(4, 4))) {
                m_redirectInternalFormat = GL_RGBA16F;
            } else if (qEnvironmentVariableIsSet("AUTOHDR_FLOAT_FBO")) {
                qWarning() << "AutoHDR Effect: float FBO requested but GL_RGBA16F unavailable";
            }
        }

        const char *formatName = m_redirectInternalFormat == GL_RGBA16F ? "GL_RGBA16F" : "GL_RGBA8";
        qInfo() << "AutoHDR Effect: redirect capture FBO format" << formatName;
        return m_redirectInternalFormat;
    }

    QString AutoHDREffect::resolveOnnxModelPath() const
    {
        if (qEnvironmentVariableIsSet("AUTOHDR_ONNX_MODEL")) {
            return qEnvironmentVariable("AUTOHDR_ONNX_MODEL");
        }
        if (qEnvironmentVariableIsSet("AUTOHDR_ONNX_V2")
            && qEnvironmentVariableIntValue("AUTOHDR_ONNX_V2") != 0) {
            const QString v2 = AutoHdr::resolveGuidanceModelPath(AutoHdr::AiGuidanceModel::GuidanceV2);
            if (!v2.isEmpty()) {
                return v2;
            }
        }

        const QString configured = AutoHdr::resolveGuidanceModelPath(m_aiGuidanceModel);
        if (!configured.isEmpty()) {
            return configured;
        }

        return AutoHdr::resolveGuidanceModelPath(AutoHdr::AiGuidanceModel::Latest);
    }

    bool AutoHDREffect::isGuidanceV2Model(const QString &modelPath) const
    {
        const std::optional<AutoHdr::GuidanceModelDescriptor> descriptor =
            AutoHdr::guidanceModelFromPath(modelPath);
        return descriptor && descriptor->usesAsyncOrt;
    }

    bool AutoHDREffect::isLiveOffscreenData(const OffscreenWindowData *data) const
    {
        if (!data) {
            return false;
        }
        for (const auto &entry : m_offscreenWindows) {
            if (entry.second.get() == data) {
                return true;
            }
        }
        return false;
    }

    void AutoHDREffect::invalidateAsyncGuidanceFor(OffscreenWindowData *data)
    {
        const bool matchAll = data == nullptr;
        bool hadMatch = matchAll;

        if (matchAll || m_asyncGuidanceTarget == data) {
            m_asyncGuidanceTarget = nullptr;
            ++m_asyncGuidanceGeneration;
            hadMatch = true;
        }
        if (matchAll || m_ortPboReadTarget == data) {
            m_ortPboReadPending = false;
            m_ortPboReadTarget = nullptr;
            m_ortPboReadW = 0;
            m_ortPboReadH = 0;
            hadMatch = true;
        }
        if (hadMatch && m_guidanceWorker) {
            m_guidanceWorker->cancelInflight();
        }
    }

    void AutoHDREffect::invalidateAsyncGuidanceForWindow(EffectWindow *window)
    {
        if (!window) {
            return;
        }
        const auto it = m_offscreenWindows.find(window);
        if (it == m_offscreenWindows.end()) {
            return;
        }
        invalidateAsyncGuidanceFor(it->second.get());
    }

    void AutoHDREffect::initGuidanceInference()
    {
        invalidateAsyncGuidanceFor(nullptr);

        if (m_guidanceWorker) {
            m_guidanceWorker->shutdown();
            m_guidanceWorker.reset();
        }

        if (m_aiForceDisabled) {
            m_guidanceInference.reset();
            m_guidanceBackendName = QStringLiteral("disabled");
            return;
        }

        const QString modelPath = resolveOnnxModelPath();
        m_guidanceInference = AutoHdr::createOnnxGuidanceInference(modelPath);
        m_guidanceBackendName =
            m_guidanceInference ? m_guidanceInference->backendName() : QStringLiteral("none");

        if (useAsyncGuidanceInference() && m_guidanceInference && m_guidanceInference->isAvailable()) {
            m_guidanceWorker = std::make_unique<AutoHdr::GuidanceWorker>(m_guidanceInference.get());
            m_guidanceWorker->start();
            if (!m_loggedV2Active) {
                m_loggedV2Active = true;
                qInfo() << "AutoHDR Effect: async guidance active for"
                        << AutoHdr::aiGuidanceModelToString(m_aiGuidanceModel) << modelPath;
            }
            qInfo() << "AutoHDR Effect: async guidance worker started for" << modelPath;
        }

        qInfo() << "AutoHDR Effect: guidance backend" << m_guidanceBackendName;
    }

    bool AutoHDREffect::useAsyncGuidanceInference() const
    {
        return useOnnxOrtReadback() && isGuidanceV2Model(resolveOnnxModelPath());
    }

    QSize AutoHDREffect::capGuidanceSizeForOrt(const QSize &size) const
    {
        constexpr int kMaxOrtPixels = 1920 * 1080;
        const int pixels = size.width() * size.height();
        if (pixels <= kMaxOrtPixels) {
            return size;
        }
        const float scale = std::sqrt(static_cast<float>(kMaxOrtPixels) / static_cast<float>(pixels));
        return QSize(qMax(1, static_cast<int>(size.width() * scale)),
                     qMax(1, static_cast<int>(size.height() * scale)));
    }

    bool AutoHDREffect::shouldSubmitOrt(OffscreenWindowData *offscreenData)
    {
        if (!offscreenData || !offscreenData->contentDamaged) {
            return false;
        }
        if (m_frameBudgetSkipOrt) {
            return false;
        }
        const int interval = AutoHdr::aiOrtSubmitInterval(m_aiQuality);
        if (interval > 1 && (m_guidanceFrameCounter % interval) != 0) {
            return false;
        }
        return !m_guidanceWorker || !m_guidanceWorker->hasInflightWork();
    }

    void AutoHDREffect::ensureOrtPbo(int mapW, int mapH)
    {
        const size_t byteSize = static_cast<size_t>(mapW) * mapH * 4 * sizeof(float);
        if (m_ortPbo[0] == 0) {
            glGenBuffers(2, m_ortPbo);
        }
        for (GLuint pbo : m_ortPbo) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
            glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(byteSize), nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    void AutoHDREffect::finishPendingOrtReadback()
    {
        if (!m_ortPboReadPending || !m_ortPboReadTarget || m_ortPboReadW <= 0 || m_ortPboReadH <= 0
            || !m_guidanceWorker) {
            m_ortPboReadPending = false;
            return;
        }

        if (!isLiveOffscreenData(m_ortPboReadTarget)) {
            m_ortPboReadPending = false;
            m_ortPboReadTarget = nullptr;
            m_ortPboReadW = 0;
            m_ortPboReadH = 0;
            return;
        }

        const int readIndex = 1 - m_ortPboWriteIndex;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_ortPbo[readIndex]);
        void *mapped = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
        if (!mapped) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            m_ortPboReadPending = false;
            return;
        }

        const size_t pixelCount = static_cast<size_t>(m_ortPboReadW) * m_ortPboReadH;
        m_downsampleReadback.resize(pixelCount * 3);
        const float *rgba = static_cast<const float *>(mapped);
        for (size_t i = 0; i < pixelCount; ++i) {
            m_downsampleReadback[i * 3 + 0] = rgba[i * 4 + 0];
            m_downsampleReadback[i * 3 + 1] = rgba[i * 4 + 1];
            m_downsampleReadback[i * 3 + 2] = rgba[i * 4 + 2];
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        m_asyncGuidanceGeneration =
            m_guidanceWorker->submit(m_downsampleReadback.data(), m_ortPboReadW, m_ortPboReadH);
        m_asyncGuidanceTarget = m_ortPboReadTarget;
        m_ortPboReadPending = false;
    }

    void AutoHDREffect::issueOrtInputReadback(OffscreenWindowData *offscreenData, int mapW, int mapH)
    {
        if (!offscreenData || !offscreenData->ortInputFbo || mapW <= 0 || mapH <= 0) {
            return;
        }

        ensureOrtPbo(mapW, mapH);
        GLFramebuffer::pushFramebuffer(offscreenData->ortInputFbo.get());
        glBindBuffer(GL_PIXEL_PACK_BUFFER, m_ortPbo[m_ortPboWriteIndex]);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, mapW, mapH, GL_RGBA, GL_FLOAT, 0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        GLFramebuffer::popFramebuffer();

        m_ortPboWriteIndex = 1 - m_ortPboWriteIndex;
        m_ortPboReadPending = true;
        m_ortPboReadTarget = offscreenData;
        m_ortPboReadW = mapW;
        m_ortPboReadH = mapH;
    }

    bool AutoHDREffect::readOrtInputRgb(OffscreenWindowData *offscreenData, int mapW, int mapH)
    {
        if (!offscreenData || !offscreenData->ortInputFbo || mapW <= 0 || mapH <= 0) {
            return false;
        }

        GLFramebuffer::pushFramebuffer(offscreenData->ortInputFbo.get());
        const size_t pixelCount = static_cast<size_t>(mapW) * mapH;
        m_downsampleReadbackFloat.resize(pixelCount * 4);
        m_downsampleReadback.resize(pixelCount * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, mapW, mapH, GL_RGBA, GL_FLOAT, m_downsampleReadbackFloat.data());
        for (size_t i = 0; i < pixelCount; ++i) {
            m_downsampleReadback[i * 3 + 0] = m_downsampleReadbackFloat[i * 4 + 0];
            m_downsampleReadback[i * 3 + 1] = m_downsampleReadbackFloat[i * 4 + 1];
            m_downsampleReadback[i * 3 + 2] = m_downsampleReadbackFloat[i * 4 + 2];
        }
        GLFramebuffer::popFramebuffer();
        return true;
    }

    void AutoHDREffect::renderGuidanceBlendPass(OffscreenWindowData *offscreenData, int mapW, int mapH)
    {
        if (!offscreenData || !offscreenData->guidanceFbo || !offscreenData->ortOutputTexture
            || !offscreenData->guidanceTexture || !offscreenData->guidanceScratchFbo
            || !m_guidanceBlendShader || !m_downsampleShader) {
            return;
        }

        renderTexturePass(offscreenData->guidanceTexture.get(), offscreenData->guidanceScratchFbo.get(),
                          m_downsampleShader.get(), offscreenData->guidanceSize, mapW, mapH);

        GLFramebuffer::pushFramebuffer(offscreenData->guidanceFbo.get());
        glViewport(0, 0, mapW, mapH);

        ShaderBinder binder(m_guidanceBlendShader.get());
        QMatrix4x4 mvp;
        mvp.ortho(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
        m_guidanceBlendShader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

        const int glslLoc = m_guidanceBlendShader->uniformLocation("glslSampler");
        if (glslLoc >= 0) {
            glActiveTexture(GL_TEXTURE1);
            offscreenData->guidanceScratchTexture->bind();
            m_guidanceBlendShader->setUniform(glslLoc, 1);
            glActiveTexture(GL_TEXTURE0);
        }

        offscreenData->ortOutputTexture->bind();

        GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
        vbo->reset();
        vbo->setAttribLayout(std::span(GLVertexBuffer::GLVertex2DLayout), sizeof(GLVertex2D));
        const auto map = vbo->map<GLVertex2D>(4);
        if (!map) {
            GLFramebuffer::popFramebuffer();
            return;
        }
        (*map)[0] = GLVertex2D{QVector2D(-1.0f, -1.0f), QVector2D(0.0f, 0.0f)};
        (*map)[1] = GLVertex2D{QVector2D(1.0f, -1.0f), QVector2D(1.0f, 0.0f)};
        (*map)[2] = GLVertex2D{QVector2D(-1.0f, 1.0f), QVector2D(0.0f, 1.0f)};
        (*map)[3] = GLVertex2D{QVector2D(1.0f, 1.0f), QVector2D(1.0f, 1.0f)};
        vbo->unmap();
        vbo->bindArrays();
        vbo->draw(Region::infinite(), GL_TRIANGLE_STRIP, 0, 4, false);
        vbo->unbindArrays();
        offscreenData->ortOutputTexture->unbind();
        GLFramebuffer::popFramebuffer();
    }

    void AutoHDREffect::uploadOrtOutputAndBlend(OffscreenWindowData *offscreenData, const float *rgba, int ortW,
                                                int ortH)
    {
        if (!offscreenData || !offscreenData->ortOutputTexture || !rgba || ortW <= 0 || ortH <= 0) {
            return;
        }
        if (!isLiveOffscreenData(offscreenData)) {
            return;
        }

        offscreenData->ortOutputTexture->bind();
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ortW, ortH, GL_RGBA, GL_FLOAT, rgba);
        offscreenData->ortOutputTexture->unbind();

        const int mapW = offscreenData->guidanceSize.width();
        const int mapH = offscreenData->guidanceSize.height();
        renderGuidanceBlendPass(offscreenData, mapW, mapH);
        offscreenData->guidanceValid = true;
        offscreenData->guidanceEverUploaded = true;
    }

    void AutoHDREffect::pollAsyncGuidanceResults()
    {
        if (!m_guidanceWorker) {
            return;
        }

        if (m_guidanceWorker->takeFailedJob()) {
            m_asyncGuidanceTarget = nullptr;
            if (!m_loggedAsyncFailure) {
                m_loggedAsyncFailure = true;
                qWarning() << "AutoHDR Effect: async ORT failed, keeping GLSL map";
            }
            return;
        }

        std::vector<float> rgba;
        int width = 0;
        int height = 0;
        uint64_t resultGeneration = 0;
        if (!m_guidanceWorker->tryTakeResult(&rgba, &width, &height, &resultGeneration)) {
            return;
        }

        OffscreenWindowData *target = m_asyncGuidanceTarget;
        if (!target || !isLiveOffscreenData(target) || resultGeneration != m_asyncGuidanceGeneration
            || !target->guidanceTexture || target->ortInferenceSize.width() != width
            || target->ortInferenceSize.height() != height) {
            if (target && !isLiveOffscreenData(target)) {
                m_asyncGuidanceTarget = nullptr;
            }
            return;
        }

        uploadOrtOutputAndBlend(target, rgba.data(), width, height);
    }

    void AutoHDREffect::invalidateAllGuidance()
    {
        for (auto &entry : m_offscreenWindows) {
            if (!entry.second) {
                continue;
            }
            entry.second->guidanceValid = false;
            entry.second->guidanceEverUploaded = false;
            entry.second->needsGuidanceUpdate = true;
        }
    }

    QString AutoHDREffect::activeGuidanceBackendLabel() const
    {
        if (m_aiForceDisabled) {
            return QStringLiteral("disabled");
        }
        if (m_aiBackend == AutoHdr::AiBackend::GlslOnly || !preferOnnxGuidance()) {
            return QStringLiteral("GLSL");
        }
        if (useOnnxGpuFastPath()) {
            return QStringLiteral("onnx-style-gpu");
        }
        if (useAsyncGuidanceInference()) {
            return QStringLiteral("onnx-async");
        }
        return m_guidanceBackendName.isEmpty() ? QStringLiteral("onnx") : m_guidanceBackendName;
    }

    QString AutoHDREffect::enabledStatusMessage(const CalibrationSettings &settings) const
    {
        if (!shouldUseAi(settings)) {
            return QStringLiteral("AutoHDR enabled");
        }
        return QStringLiteral("AutoHDR enabled — AI: %1").arg(activeGuidanceBackendLabel());
    }

    bool AutoHDREffect::shouldUseAi(const CalibrationSettings &settings) const
    {
        if (m_aiForceDisabled || !m_aiEnabledGlobal) {
            return false;
        }
        return settings.aiEnhanced;
    }

    bool AutoHDREffect::preferOnnxGuidance() const
    {
        if (m_aiBackend == AutoHdr::AiBackend::GlslOnly) {
            return false;
        }
        if (m_aiBackend == AutoHdr::AiBackend::Onnx) {
            return true;
        }
        // Auto: prefer ONNX-style path when ORT was built or v0 GPU formula is available.
        return AutoHdr::onnxGuidanceRuntimeBuilt()
            || m_guidanceBackendName.startsWith(QStringLiteral("onnx"));
    }

    bool AutoHDREffect::useOnnxGpuFastPath() const
    {
        if (!preferOnnxGuidance()) {
            return false;
        }
        // Real ORT+readback only when explicitly forced (for future non-v0 models).
        if (qEnvironmentVariableIsSet("AUTOHDR_ONNX_FORCE_ORT")
            && qEnvironmentVariableIntValue("AUTOHDR_ONNX_FORCE_ORT") != 0) {
            return false;
        }
        const QString modelPath = resolveOnnxModelPath();
        const std::optional<AutoHdr::GuidanceModelDescriptor> descriptor =
            AutoHdr::guidanceModelFromPath(modelPath);
        if (descriptor) {
            return descriptor->usesGlslFormula;
        }
        return modelPath.isEmpty();
    }

    bool AutoHDREffect::useOnnxOrtReadback() const
    {
        return preferOnnxGuidance() && !useOnnxGpuFastPath() && m_guidanceInference
            && m_guidanceInference->isAvailable();
    }

    QByteArray AutoHDREffect::loadSimpleShaderSource(const QString &fileName) const
    {
        const QString path = AutoHdr::locateEffectDataFile(QStringLiteral("kwin/effects/autohdr/") + fileName);
        if (path.isEmpty()) {
            return {};
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return file.readAll();
    }

    bool AutoHDREffect::loadPassShaders()
    {
        if (!effects->makeOpenGLContextCurrent()) {
            return false;
        }

        if (!m_downsampleShader) {
            const QByteArray source = loadSimpleShaderSource(QStringLiteral("autohdr_downsample.frag"));
            if (!source.isEmpty()) {
                m_downsampleShader =
                    ShaderManager::instance()->generateCustomShader(ShaderTrait::MapTexture, QByteArray(), source);
            }
            if (!m_downsampleShader) {
                qWarning() << "AutoHDR Effect: failed to compile downsample shader";
            }
        }

        if (!m_guidanceShader) {
            const QByteArray source = loadSimpleShaderSource(QStringLiteral("autohdr_guidance.frag"));
            if (!source.isEmpty()) {
                m_guidanceShader =
                    ShaderManager::instance()->generateCustomShader(ShaderTrait::MapTexture, QByteArray(), source);
            }
            if (!m_guidanceShader) {
                qWarning() << "AutoHDR Effect: failed to compile guidance shader";
            }
        }

        if (!m_guidanceBlendShader) {
            const QByteArray source = loadSimpleShaderSource(QStringLiteral("autohdr_guidance_blend.frag"));
            if (!source.isEmpty()) {
                m_guidanceBlendShader =
                    ShaderManager::instance()->generateCustomShader(ShaderTrait::MapTexture, QByteArray(), source);
            }
            if (!m_guidanceBlendShader) {
                qWarning() << "AutoHDR Effect: failed to compile guidance blend shader";
            }
        }

        if (!m_neutralGuidanceTexture) {
            m_neutralGuidanceTexture = GLTexture::allocate(GL_RGBA16F, QSize(1, 1));
            if (!m_neutralGuidanceTexture) {
                m_neutralGuidanceTexture = GLTexture::allocate(GL_RGBA8, QSize(1, 1));
            }
            if (m_neutralGuidanceTexture) {
                m_neutralGuidanceTexture->setFilter(GL_LINEAR);
                m_neutralGuidanceTexture->setWrapMode(GL_CLAMP_TO_EDGE);
                // Neutral: no highlight expansion, no shadow/depth masks.
                const float neutral[4] = {1.0f, 0.0f, 0.0f, 0.0f};
                m_neutralGuidanceTexture->bind();
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_FLOAT, neutral);
                m_neutralGuidanceTexture->unbind();
            }
        }

        return m_downsampleShader && m_guidanceShader && m_neutralGuidanceTexture;
    }

    bool AutoHDREffect::ensureAiResources(OffscreenWindowData *offscreenData, const QSize &windowSize)
    {
        if (!offscreenData || windowSize.isEmpty()) {
            return false;
        }
        if (!loadPassShaders()) {
            return false;
        }

        const int scale = AutoHdr::aiGuidanceScale(m_aiQuality);
        QSize guidanceSize(qMax(1, windowSize.width() / scale), qMax(1, windowSize.height() / scale));
        const bool asyncV2 = useAsyncGuidanceInference();
        if (useOnnxOrtReadback() && !asyncV2) {
            const QSize capped = capGuidanceSizeForOrt(guidanceSize);
            if (capped != guidanceSize) {
                if (!m_loggedOrtCapWarning) {
                    m_loggedOrtCapWarning = true;
                    qWarning() << "AutoHDR Effect: capping ORT guidance map to" << capped
                               << "for performance (Quality tier on large windows)";
                }
                guidanceSize = capped;
            }
        }
        const QSize ortSize = asyncV2 ? AutoHdr::aiOrtInferenceSize(m_aiQuality) : QSize();
        if (offscreenData->guidanceSize != guidanceSize) {
            offscreenData->downsampleTexture.reset();
            offscreenData->downsampleFbo.reset();
            offscreenData->guidanceTexture.reset();
            offscreenData->guidanceFbo.reset();
            offscreenData->guidanceScratchTexture.reset();
            offscreenData->guidanceScratchFbo.reset();
            offscreenData->guidanceValid = false;
            offscreenData->guidanceEverUploaded = false;
            offscreenData->needsGuidanceUpdate = true;
            offscreenData->guidanceSize = guidanceSize;
        }
        if (asyncV2 && offscreenData->ortInferenceSize != ortSize) {
            offscreenData->ortInputTexture.reset();
            offscreenData->ortInputFbo.reset();
            offscreenData->ortOutputTexture.reset();
            offscreenData->ortInferenceSize = ortSize;
        }

        if (!offscreenData->downsampleTexture) {
            offscreenData->downsampleTexture = GLTexture::allocate(GL_RGBA16F, guidanceSize);
            offscreenData->downsampleFloat = static_cast<bool>(offscreenData->downsampleTexture);
            if (!offscreenData->downsampleTexture) {
                offscreenData->downsampleTexture = GLTexture::allocate(GL_RGBA8, guidanceSize);
                offscreenData->downsampleFloat = false;
            }
            if (!offscreenData->downsampleTexture) {
                return false;
            }
            offscreenData->downsampleTexture->setFilter(GL_LINEAR);
            offscreenData->downsampleTexture->setWrapMode(GL_CLAMP_TO_EDGE);
            offscreenData->downsampleFbo =
                std::make_unique<GLFramebuffer>(offscreenData->downsampleTexture.get());
        }

        if (!offscreenData->guidanceTexture) {
            offscreenData->guidanceTexture = GLTexture::allocate(GL_RGBA16F, guidanceSize);
            if (!offscreenData->guidanceTexture) {
                offscreenData->guidanceTexture = GLTexture::allocate(GL_RGBA8, guidanceSize);
            }
            if (!offscreenData->guidanceTexture) {
                return false;
            }
            offscreenData->guidanceTexture->setFilter(GL_LINEAR);
            offscreenData->guidanceTexture->setWrapMode(GL_CLAMP_TO_EDGE);
            offscreenData->guidanceFbo = std::make_unique<GLFramebuffer>(offscreenData->guidanceTexture.get());

            offscreenData->guidanceScratchTexture = GLTexture::allocate(GL_RGBA16F, guidanceSize);
            if (!offscreenData->guidanceScratchTexture) {
                offscreenData->guidanceScratchTexture = GLTexture::allocate(GL_RGBA8, guidanceSize);
            }
            if (offscreenData->guidanceScratchTexture) {
                offscreenData->guidanceScratchTexture->setFilter(GL_LINEAR);
                offscreenData->guidanceScratchTexture->setWrapMode(GL_CLAMP_TO_EDGE);
                offscreenData->guidanceScratchFbo =
                    std::make_unique<GLFramebuffer>(offscreenData->guidanceScratchTexture.get());
            }
        }

        if (asyncV2 && ortSize.isValid() && !offscreenData->ortInputTexture) {
            offscreenData->ortInputTexture = GLTexture::allocate(GL_RGBA16F, ortSize);
            if (!offscreenData->ortInputTexture) {
                offscreenData->ortInputTexture = GLTexture::allocate(GL_RGBA8, ortSize);
            }
            if (!offscreenData->ortInputTexture) {
                return false;
            }
            offscreenData->ortInputTexture->setFilter(GL_LINEAR);
            offscreenData->ortInputTexture->setWrapMode(GL_CLAMP_TO_EDGE);
            offscreenData->ortInputFbo =
                std::make_unique<GLFramebuffer>(offscreenData->ortInputTexture.get());

            offscreenData->ortOutputTexture = GLTexture::allocate(GL_RGBA16F, ortSize);
            if (!offscreenData->ortOutputTexture) {
                offscreenData->ortOutputTexture = GLTexture::allocate(GL_RGBA8, ortSize);
            }
            if (!offscreenData->ortOutputTexture) {
                return false;
            }
            offscreenData->ortOutputTexture->setFilter(GL_LINEAR);
            offscreenData->ortOutputTexture->setWrapMode(GL_CLAMP_TO_EDGE);
        }

        return true;
    }

    void AutoHDREffect::renderTexturePass(GLTexture *source, GLFramebuffer *target, GLShader *shader,
                                          const QSize &targetSize, int sourceWidth, int sourceHeight)
    {
        if (!source || !target || !shader || targetSize.isEmpty()) {
            return;
        }

        GLFramebuffer::pushFramebuffer(target);
        glViewport(0, 0, targetSize.width(), targetSize.height());

        ShaderBinder binder(shader);
        QMatrix4x4 mvp;
        mvp.ortho(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
        shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
        shader->setUniform(GLShader::IntUniform::TextureWidth, sourceWidth);
        shader->setUniform(GLShader::IntUniform::TextureHeight, sourceHeight);

        const int locWidth = shader->uniformLocation("textureWidth");
        const int locHeight = shader->uniformLocation("textureHeight");
        if (locWidth >= 0) {
            shader->setUniform(locWidth, sourceWidth);
        }
        if (locHeight >= 0) {
            shader->setUniform(locHeight, sourceHeight);
        }

        GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
        vbo->reset();
        vbo->setAttribLayout(std::span(GLVertexBuffer::GLVertex2DLayout), sizeof(GLVertex2D));
        const auto map = vbo->map<GLVertex2D>(4);
        if (!map) {
            GLFramebuffer::popFramebuffer();
            return;
        }

        // Identity UVs in GL texture space (0,0 = bottom-left). The main pass samples
        // guidanceMap with the same texcoord0 used for the window texture (already
        // content-transform adjusted). Baking source->matrix() here double-applies the
        // Y-flip and produces upside-down, misaligned highlight ghosts.
        (*map)[0] = GLVertex2D{QVector2D(-1.0f, -1.0f), QVector2D(0.0f, 0.0f)};
        (*map)[1] = GLVertex2D{QVector2D(1.0f, -1.0f), QVector2D(1.0f, 0.0f)};
        (*map)[2] = GLVertex2D{QVector2D(-1.0f, 1.0f), QVector2D(0.0f, 1.0f)};
        (*map)[3] = GLVertex2D{QVector2D(1.0f, 1.0f), QVector2D(1.0f, 1.0f)};
        vbo->unmap();
        vbo->bindArrays();

        source->bind();
        vbo->draw(Region::infinite(), GL_TRIANGLE_STRIP, 0, 4, false);
        source->unbind();
        vbo->unbindArrays();

        GLFramebuffer::popFramebuffer();
    }

    void AutoHDREffect::uploadGuidanceTexture(OffscreenWindowData *offscreenData, const float *rgba, int width,
                                              int height)
    {
        if (!offscreenData || !offscreenData->guidanceTexture || !rgba || width <= 0 || height <= 0) {
            return;
        }
        offscreenData->guidanceTexture->bind();
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, rgba);
        offscreenData->guidanceTexture->unbind();
        offscreenData->guidanceValid = true;
        offscreenData->guidanceEverUploaded = true;
    }

    bool AutoHDREffect::readDownsampleRgb(OffscreenWindowData *offscreenData, int mapW, int mapH)
    {
        if (!offscreenData || !offscreenData->downsampleFbo || mapW <= 0 || mapH <= 0) {
            return false;
        }

        GLFramebuffer::pushFramebuffer(offscreenData->downsampleFbo.get());
        const size_t pixelCount = static_cast<size_t>(mapW) * mapH;
        m_downsampleReadback.resize(pixelCount * 3);

        const bool floatReadback =
            offscreenData->downsampleFloat || captureInternalFormat() == GL_RGBA16F;
        if (floatReadback) {
            m_downsampleReadbackFloat.resize(pixelCount * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, mapW, mapH, GL_RGBA, GL_FLOAT, m_downsampleReadbackFloat.data());
            for (size_t i = 0; i < pixelCount; ++i) {
                m_downsampleReadback[i * 3 + 0] = m_downsampleReadbackFloat[i * 4 + 0];
                m_downsampleReadback[i * 3 + 1] = m_downsampleReadbackFloat[i * 4 + 1];
                m_downsampleReadback[i * 3 + 2] = m_downsampleReadbackFloat[i * 4 + 2];
            }
        } else {
            if (!m_loggedOrtUbyteWarning) {
                m_loggedOrtUbyteWarning = true;
                qWarning() << "AutoHDR Effect: ONNX readback using 8-bit pixels; enable float FBO for best results";
            }
            m_downsampleUbyte.resize(pixelCount * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, mapW, mapH, GL_RGBA, GL_UNSIGNED_BYTE, m_downsampleUbyte.data());
            for (size_t i = 0; i < pixelCount; ++i) {
                m_downsampleReadback[i * 3 + 0] = m_downsampleUbyte[i * 4 + 0] * (1.0f / 255.0f);
                m_downsampleReadback[i * 3 + 1] = m_downsampleUbyte[i * 4 + 1] * (1.0f / 255.0f);
                m_downsampleReadback[i * 3 + 2] = m_downsampleUbyte[i * 4 + 2] * (1.0f / 255.0f);
            }
        }
        GLFramebuffer::popFramebuffer();
        return true;
    }

    bool AutoHDREffect::readGuidanceMapRgba(OffscreenWindowData *offscreenData, int mapW, int mapH,
                                            std::vector<float> &rgba)
    {
        if (!offscreenData || !offscreenData->guidanceFbo || mapW <= 0 || mapH <= 0) {
            return false;
        }

        GLFramebuffer::pushFramebuffer(offscreenData->guidanceFbo.get());
        const size_t pixelCount = static_cast<size_t>(mapW) * mapH;
        rgba.resize(pixelCount * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, mapW, mapH, GL_RGBA, GL_FLOAT, rgba.data());
        GLFramebuffer::popFramebuffer();
        return true;
    }

    void AutoHDREffect::blendGuidanceWithGlslFloor(std::vector<float> &rgba,
                                                   const std::vector<float> &glslFloor) const
    {
        if (glslFloor.size() != rgba.size()) {
            return;
        }
        for (size_t i = 0; i < rgba.size() / 4; ++i) {
            rgba[i * 4 + 3] = std::max(rgba[i * 4 + 3], glslFloor[i * 4 + 3]);
        }
    }

    void AutoHDREffect::updateGuidanceMap(EffectWindow *window, OffscreenWindowData *offscreenData,
                                          const CalibrationSettings &settings)
    {
        if (!window || m_pendingUnredirects.contains(window)) {
            return;
        }
        if (!offscreenData || !offscreenData->texture || !shouldUseAi(settings)) {
            return;
        }

        const QSize windowSize = offscreenData->texture->size();
        if (windowSize.width() < 32 || windowSize.height() < 32) {
            return;
        }
        if (!ensureAiResources(offscreenData, windowSize)) {
            if (!m_loggedAiResourcesFailure) {
                m_loggedAiResourcesFailure = true;
                qWarning() << "AutoHDR Effect: AI resources unavailable"
                           << "(downsample=" << (m_downsampleShader != nullptr)
                           << "guidance=" << (m_guidanceShader != nullptr)
                           << "blend=" << (m_guidanceBlendShader != nullptr) << ")";
            }
            return;
        }

        const bool asyncV2 =
            useAsyncGuidanceInference() && m_guidanceWorker && offscreenData->downsampleFbo;

        if (offscreenData->guidanceValid && !offscreenData->needsGuidanceUpdate) {
            if (!asyncV2) {
                const int staticInterval = AutoHdr::aiInferenceInterval(m_aiQuality);
                if (staticInterval <= 1 || (m_guidanceFrameCounter % staticInterval) != 0) {
                    return;
                }
            } else {
                return;
            }
        }

        if (asyncV2 && offscreenData->guidanceValid && offscreenData->needsGuidanceUpdate
            && !offscreenData->contentDamaged) {
            offscreenData->needsGuidanceUpdate = false;
            return;
        }

        const QSize guidanceSize = offscreenData->guidanceSize;
        const int mapW = guidanceSize.width();
        const int mapH = guidanceSize.height();
        const bool fullMap = guidanceSize == windowSize;

        GLTexture *analysisSource = offscreenData->texture.get();
        const bool needDownsample = !fullMap || useOnnxOrtReadback();
        if (needDownsample) {
            renderTexturePass(offscreenData->texture.get(), offscreenData->downsampleFbo.get(),
                              m_downsampleShader.get(), guidanceSize, mapW, mapH);
            analysisSource = offscreenData->downsampleTexture.get();
        }

        // GLSL v1.5 bootstrap: produce a valid guidance map when content changes or on interval refresh.
        renderTexturePass(analysisSource, offscreenData->guidanceFbo.get(), m_guidanceShader.get(), guidanceSize,
                          mapW, mapH);
        offscreenData->guidanceValid = true;
        offscreenData->guidanceEverUploaded = true;
        offscreenData->needsGuidanceUpdate = false;

        if (!m_loggedGuidanceBackend) {
            m_loggedGuidanceBackend = true;
            qInfo() << "AutoHDR Effect: AI guidance active via" << activeGuidanceBackendLabel();
        }

        // Optional v2 async ORT refinement (GLSL map is current; ORT applied only when fresh).
        if (asyncV2) {
            const int ortW = offscreenData->ortInferenceSize.width();
            const int ortH = offscreenData->ortInferenceSize.height();
            if (ortW > 0 && ortH > 0 && offscreenData->ortInputFbo) {
                renderTexturePass(offscreenData->texture.get(), offscreenData->ortInputFbo.get(),
                                  m_downsampleShader.get(), offscreenData->ortInferenceSize, ortW, ortH);
                if (shouldSubmitOrt(offscreenData)) {
                    issueOrtInputReadback(offscreenData, ortW, ortH);
                    offscreenData->contentDamaged = false;
                }
            }
            return;
        }

        // Sync ORT readback (AUTOHDR_ONNX_FORCE_ORT or non-v2 models).
        if (useOnnxOrtReadback() && offscreenData->downsampleFbo) {
            if (!readDownsampleRgb(offscreenData, mapW, mapH)) {
                return;
            }

            const size_t pixelCount = static_cast<size_t>(mapW) * mapH;
            m_guidanceUpload.resize(pixelCount * 4);
            if (m_guidanceInference->run(m_downsampleReadback.data(), mapW, mapH, m_guidanceUpload.data())) {
                readGuidanceMapRgba(offscreenData, mapW, mapH, m_glslGuidanceFloor);
                blendGuidanceWithGlslFloor(m_guidanceUpload, m_glslGuidanceFloor);
                uploadGuidanceTexture(offscreenData, m_guidanceUpload.data(), mapW, mapH);
                return;
            }
            qWarning() << "AutoHDR Effect: ONNX guidance failed; keeping GLSL formula map";
        }
    }

    void AutoHDREffect::scheduleActiveHdrRepaints()
    {
        constexpr qint64 kGuidanceHeartbeatMs = 33;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = m_activeWindows.constBegin(); it != m_activeWindows.constEnd(); ++it) {
            EffectWindow *window = it.key();
            if (!window || !AutoHdr::windowOnHdrOutput(window)) {
                continue;
            }

            const auto offIt = m_offscreenWindows.find(window);
            if (offIt != m_offscreenWindows.end()) {
                OffscreenWindowData *offscreenData = offIt->second.get();
                const bool heartbeat =
                    offscreenData->lastCaptureMs == 0
                    || (now - offscreenData->lastCaptureMs) >= kGuidanceHeartbeatMs;
                if (heartbeat && !offscreenData->isDirty) {
                    offscreenData->isDirty = true;
                }
            }
        }
    }

    bool AutoHDREffect::anyActiveHdrWindow() const
    {
        for (auto it = m_activeWindows.constBegin(); it != m_activeWindows.constEnd(); ++it) {
            if (it.key() && AutoHdr::windowOnHdrOutput(it.key())) {
                return true;
            }
        }
        return false;
    }

    void AutoHDREffect::ensureCompositorHeartbeat()
    {
        if (m_activeWindows.isEmpty()) {
            m_compositorHeartbeat.stop();
            return;
        }
        if (!m_compositorHeartbeat.isActive()) {
            m_compositorHeartbeat.start();
        }
    }

    void AutoHDREffect::onCompositorHeartbeat()
    {
        if (m_activeWindows.isEmpty()) {
            m_compositorHeartbeat.stop();
            return;
        }

        bool needsGlobalRepaint = false;
        for (auto it = m_activeWindows.constBegin(); it != m_activeWindows.constEnd(); ++it) {
            EffectWindow *window = it.key();
            if (!window || !AutoHdr::windowOnHdrOutput(window)) {
                continue;
            }

            if (isBrowserWindow(window)) {
                const auto offIt = m_offscreenWindows.find(window);
                if (offIt != m_offscreenWindows.end()) {
                    ensureWindowRedirect(window, offIt->second.get());
                }
                if (WindowItem *windowItem = window->windowItem()) {
                    windowItem->scheduleSceneRepaint(windowItem->boundingRect());
                    if (SurfaceItem *surfaceItem = windowItem->surfaceItem()) {
                        surfaceItem->scheduleSceneRepaint(surfaceItem->boundingRect());
                    }
                }
            }

            window->addRepaintFull();
            effects->addRepaint(window->expandedGeometry());
            needsGlobalRepaint = true;
        }
        if (needsGlobalRepaint) {
            effects->addRepaintFull();
        }
    }

    void AutoHDREffect::setupOffscreenConnections()
    {
        if (m_windowDeletedConnection) {
            return;
        }
        m_windowDeletedConnection =
            connect(effects, &EffectsHandler::windowDeleted, this, &AutoHDREffect::handleWindowDeleted);
    }

    void AutoHDREffect::destroyOffscreenConnections()
    {
        disconnect(m_windowDeletedConnection);
        m_windowDeletedConnection = {};
    }

    void AutoHDREffect::connectOutputTracking()
    {
        if (m_outputTrackingConnected) {
            return;
        }
        m_outputTrackingConnected = true;

        rebindOutputHdrConnections();
        m_screenListConnections.append(connect(effects, &EffectsHandler::screenAdded, this,
                                               [this](LogicalOutput *) {
                                                   rebindOutputHdrConnections();
                                                   onOutputConfigurationChanged();
                                               }));
        m_screenListConnections.append(connect(effects, &EffectsHandler::screenRemoved, this,
                                               [this](LogicalOutput *) {
                                                   rebindOutputHdrConnections();
                                                   onOutputConfigurationChanged();
                                               }));
    }

    void AutoHDREffect::disconnectOutputTracking()
    {
        if (!m_outputTrackingConnected) {
            return;
        }

        for (const QMetaObject::Connection &connection : std::as_const(m_outputHdrConnections)) {
            disconnect(connection);
        }
        m_outputHdrConnections.clear();

        for (const QMetaObject::Connection &connection : std::as_const(m_screenListConnections)) {
            disconnect(connection);
        }
        m_screenListConnections.clear();

        m_outputTrackingConnected = false;
    }

    void AutoHDREffect::rebindOutputHdrConnections()
    {
        for (const QMetaObject::Connection &connection : std::as_const(m_outputHdrConnections)) {
            disconnect(connection);
        }
        m_outputHdrConnections.clear();

        const QList<LogicalOutput *> outputs = effects->screens();
        for (LogicalOutput *output : outputs) {
            if (!output) {
                continue;
            }
            BackendOutput *backend = output->backendOutput();
            if (!backend) {
                continue;
            }
            m_outputHdrConnections.append(connect(backend, &BackendOutput::highDynamicRangeChanged, this,
                                                  &AutoHDREffect::onOutputConfigurationChanged));
            m_outputHdrConnections.append(connect(backend, &BackendOutput::colorDescriptionChanged, this,
                                                  &AutoHDREffect::onOutputConfigurationChanged));
        }
    }

    void AutoHDREffect::connectWindowOutputTracking(EffectWindow *window)
    {
        if (!window || m_windowOutputConnections.contains(window)) {
            return;
        }

        const auto onOutputChanged = [this, window]() {
            onWindowOutputChanged(window);
        };

        QMetaObject::Connection connection;
        if (Window *coreWindow = window->window()) {
            connection = connect(coreWindow, &Window::outputChanged, this, onOutputChanged);
        } else {
            connection = connect(window, &EffectWindow::windowFrameGeometryChanged, this,
                                 [onOutputChanged](EffectWindow *, const RectF &) {
                                     onOutputChanged();
                                 });
        }

        m_windowOutputConnections.insert(window, connection);
    }

    void AutoHDREffect::disconnectWindowOutputTracking(EffectWindow *window)
    {
        const auto it = m_windowOutputConnections.find(window);
        if (it == m_windowOutputConnections.end()) {
            return;
        }

        disconnect(it.value());
        m_windowOutputConnections.erase(it);
    }

    void AutoHDREffect::onOutputConfigurationChanged()
    {
        repaintActiveWindows();
    }

    void AutoHDREffect::onWindowOutputChanged(EffectWindow *window)
    {
        if (!window) {
            return;
        }

        window->addRepaintFull();
        effects->addRepaintFull();

        if (!m_activeWindows.contains(window)) {
            return;
        }

        const bool onHdr = AutoHdr::windowOnHdrOutput(window);
        WindowStatusToast &state = m_statusToasts[window];
        if (state.wasOnHdrOutput == onHdr) {
            return;
        }

        state.wasOnHdrOutput = onHdr;
        if (onHdr) {
            const CalibrationSettings settings = m_activeWindows.value(window, m_globalDefaults);
            showStatusToast(window, enabledStatusMessage(settings));
        } else {
            showStatusToast(window, QStringLiteral("AutoHDR suspended — SDR display"));
        }
    }

    void AutoHDREffect::handleWindowDeleted(EffectWindow *window)
    {
        if (window == m_calibratingWindow) {
            closeCalibrationOverlay(false);
        }
        removeStatusToast(window);
        disconnectWindowOutputTracking(window);
        unredirect(window);
    }

    bool AutoHDREffect::isBrowserWindow(EffectWindow *window) const
    {
        if (!window) {
            return false;
        }
        const WindowIdentifiers ids = identifiersForWindow(window);
        const auto matchesBrowser = [](const QString &value) {
            const QString lower = value.toLower();
            return lower.contains(QStringLiteral("firefox")) || lower.contains(QStringLiteral("zen"))
                || lower.contains(QStringLiteral("mozilla"));
        };
        return matchesBrowser(ids.resourceClass) || matchesBrowser(ids.windowClass);
    }

    void AutoHDREffect::applyBrowserSdrPreference(EffectWindow *window)
    {
        if (!window || !isBrowserWindow(window)) {
            return;
        }
        if (Window *core = window->window()) {
            core->setPreferredColorDescription(ColorDescription::sRGB);
        }
    }

    Item *AutoHDREffect::redirectTargetForWindow(EffectWindow *window) const
    {
        if (!window) {
            return nullptr;
        }
        WindowItem *windowItem = window->windowItem();
        if (!windowItem) {
            return nullptr;
        }
        if (SurfaceItem *surfaceItem = windowItem->surfaceItem()) {
            return surfaceItem;
        }
        return windowItem;
    }

    void AutoHDREffect::forceWindowRedirectRefresh(EffectWindow *window, OffscreenWindowData *offscreenData)
    {
        if (!window || !offscreenData) {
            return;
        }

        WindowItem *windowItem = window->windowItem();
        Item *targetItem = redirectTargetForWindow(window);
        if (!windowItem || !targetItem) {
            return;
        }

        SurfaceItem *surfaceItem = windowItem->surfaceItem();
        offscreenData->windowEffect = ItemEffect(targetItem);
        offscreenData->redirectedItem = windowItem;
        offscreenData->redirectedSurfaceItem = surfaceItem;
        offscreenData->isDirty = true;
        offscreenData->contentDamaged = true;

        if (debugPaintEnabled()) {
            qInfo() << "AutoHDR Effect: forced ItemEffect refresh for" << window->windowClass()
                    << (surfaceItem ? "surfaceItem" : "windowItem");
        }
    }

    void AutoHDREffect::logIdlePaintState(EffectWindow *window, const char *path)
    {
        if (!debugPaintEnabled() || !window) {
            return;
        }

        static QHash<EffectWindow *, qint64> lastLogMs;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 last = lastLogMs.value(window, 0);
        if (now - last < 2000) {
            return;
        }
        lastLogMs[window] = now;

        const auto offIt = m_offscreenWindows.find(window);
        WindowItem *windowItem = window->windowItem();
        SurfaceItem *surfaceItem = windowItem ? windowItem->surfaceItem() : nullptr;
        const bool redirectedSurfaceMatches =
            offIt != m_offscreenWindows.end() && offIt->second->redirectedSurfaceItem == surfaceItem;

        qInfo() << "AutoHDR Effect: paint state" << path << window->windowClass()
                << "active" << m_activeWindows.contains(window)
                << "offscreen" << (offIt != m_offscreenWindows.end())
                << "windowItem" << (windowItem != nullptr)
                << "surfaceItem" << (surfaceItem != nullptr)
                << "redirectedSurfaceMatches" << redirectedSurfaceMatches;
    }

    bool AutoHDREffect::ensureWindowRedirect(EffectWindow *window, OffscreenWindowData *offscreenData)
    {
        if (!window || !offscreenData) {
            return false;
        }

        WindowItem *windowItem = window->windowItem();
        Item *targetItem = redirectTargetForWindow(window);
        if (!windowItem || !targetItem) {
            return false;
        }

        SurfaceItem *surfaceItem = windowItem->surfaceItem();
        if (offscreenData->redirectedItem == windowItem
            && offscreenData->redirectedSurfaceItem == surfaceItem) {
            ensureSurfaceColorTracking(window, offscreenData);
            return false;
        }

        offscreenData->windowEffect = ItemEffect(targetItem);
        offscreenData->redirectedItem = windowItem;
        offscreenData->redirectedSurfaceItem = surfaceItem;
        offscreenData->isDirty = true;
        offscreenData->contentDamaged = true;
        ensureSurfaceColorTracking(window, offscreenData);

        if (debugPaintEnabled()) {
            qInfo() << "AutoHDR Effect: rebound ItemEffect for" << window->windowClass()
                    << (surfaceItem ? "surfaceItem" : "windowItem");
        }
        return true;
    }

    void AutoHDREffect::ensureBrowserSurfaceTracking(EffectWindow *window, OffscreenWindowData *offscreenData)
    {
        if (!window || !offscreenData || !isBrowserWindow(window) || offscreenData->subsurfaceTreeConnection) {
            return;
        }

        SurfaceInterface *surface = window->surface();
        if (!surface) {
            return;
        }

        offscreenData->subsurfaceTreeConnection =
            connect(surface, &SurfaceInterface::childSubSurfacesChanged, this, [this, window]() {
                if (!m_activeWindows.contains(window)) {
                    return;
                }
                const auto it = m_offscreenWindows.find(window);
                if (it == m_offscreenWindows.end()) {
                    return;
                }

                if (debugPaintEnabled()) {
                    qInfo() << "AutoHDR Effect: browser subsurface tree changed for" << window->windowClass();
                }

                OffscreenWindowData *data = it->second.get();
                forceWindowRedirectRefresh(window, data);
                window->addRepaintFull();
                effects->addRepaintFull();
            });
    }

    void AutoHDREffect::ensureSurfaceColorTracking(EffectWindow *window, OffscreenWindowData *offscreenData)
    {
        if (!window || !offscreenData) {
            return;
        }

        if (!offscreenData->surfaceColorConnection) {
            SurfaceInterface *surface = window->surface();
            if (!surface) {
                ensureBrowserSurfaceTracking(window, offscreenData);
                return;
            }

            offscreenData->surfaceColorConnection =
                connect(surface, &SurfaceInterface::colorDescriptionChanged, this, [this, window]() {
                    if (!m_activeWindows.contains(window)) {
                        return;
                    }
                    const auto it = m_offscreenWindows.find(window);
                    if (it == m_offscreenWindows.end()) {
                        return;
                    }
                    if (!isBrowserWindow(window)) {
                        return;
                    }

                    if (debugPaintEnabled()) {
                        qInfo() << "AutoHDR Effect: browser surface color changed for" << window->windowClass();
                    }

                    applyBrowserSdrPreference(window);
                    OffscreenWindowData *data = it->second.get();
                    data->isDirty = true;
                    data->contentDamaged = true;
                    forceWindowRedirectRefresh(window, data);
                    window->addRepaintFull();
                    effects->addRepaintFull();
                });
        }

        ensureBrowserSurfaceTracking(window, offscreenData);
    }

    void AutoHDREffect::redirect(EffectWindow *window)
    {
        if (!window || m_offscreenWindows.contains(window)) {
            return;
        }

        auto data = std::make_unique<OffscreenWindowData>();
        data->windowDamagedConnection =
            connect(window, &EffectWindow::windowDamaged, this, [this, window]() {
                const auto it = m_offscreenWindows.find(window);
                if (it != m_offscreenWindows.end()) {
                    it->second->isDirty = true;
                    it->second->contentDamaged = true;
                }
            });
        data->windowExpandedGeometryConnection =
            connect(window, &EffectWindow::windowExpandedGeometryChanged, this, [window]() {
                window->addRepaintFull();
            });

        m_offscreenWindows.emplace(window, std::move(data));
        ensureWindowRedirect(window, m_offscreenWindows[window].get());
        m_statusToasts[window].wasOnHdrOutput = AutoHdr::windowOnHdrOutput(window);
        connectWindowOutputTracking(window);
        if (m_offscreenWindows.size() == 1) {
            setupOffscreenConnections();
        }
    }

    void AutoHDREffect::unredirect(EffectWindow *window)
    {
        auto it = m_offscreenWindows.find(window);
        if (it == m_offscreenWindows.end()) {
            return;
        }

        if (!EglContext::currentContext()) {
            effects->openglContext()->makeCurrent();
        }

        disconnect(it->second->windowDamagedConnection);
        disconnect(it->second->windowExpandedGeometryConnection);
        disconnect(it->second->surfaceColorConnection);
        disconnect(it->second->subsurfaceTreeConnection);
        disconnectWindowOutputTracking(window);
        invalidateAsyncGuidanceFor(it->second.get());
        m_offscreenWindows.erase(it);
        if (m_offscreenWindows.empty()) {
            destroyOffscreenConnections();
        }
    }

    void AutoHDREffect::maybeRenderOffscreen(EffectWindow *window)
    {
        auto it = m_offscreenWindows.find(window);
        if (it == m_offscreenWindows.end()) {
            return;
        }

        OffscreenWindowData *offscreenData = it->second.get();
        const qreal scale = window->screen()->scale();
        const RectF logicalGeometry = snapToPixels(window->frameGeometry(), scale);
        const QSize textureSize = (logicalGeometry.size() * scale).toSize();

        if (textureSize.isEmpty()) {
            offscreenData->fbo.reset();
            offscreenData->texture.reset();
            return;
        }

        if (!offscreenData->texture || offscreenData->texture->size() != textureSize) {
            offscreenData->texture = GLTexture::allocate(captureInternalFormat(), textureSize);
            if (!offscreenData->texture) {
                return;
            }
            offscreenData->texture->setFilter(GL_LINEAR);
            offscreenData->texture->setWrapMode(GL_CLAMP_TO_EDGE);
            offscreenData->fbo = std::make_unique<GLFramebuffer>(offscreenData->texture.get());
            offscreenData->isDirty = true;
            offscreenData->contentDamaged = true;
            offscreenData->guidanceValid = false;
            offscreenData->needsGuidanceUpdate = true;
            offscreenData->downsampleTexture.reset();
            offscreenData->downsampleFbo.reset();
            offscreenData->guidanceTexture.reset();
            offscreenData->guidanceFbo.reset();
            offscreenData->guidanceSize = QSize();
        }

        if (!offscreenData->isDirty) {
            return;
        }

        RenderTarget renderTarget(offscreenData->fbo.get(), ColorDescription::sRGB);
        RenderViewport viewport(logicalGeometry, scale, renderTarget, QPoint());
        GLFramebuffer::pushFramebuffer(offscreenData->fbo.get());
        glClearColor(0.0, 0.0, 0.0, 0.0);
        glClear(GL_COLOR_BUFFER_BIT);

        WindowPaintData paintData;
        paintData.setOpacity(1.0);

        const int mask = Effect::PAINT_WINDOW_TRANSFORMED | Effect::PAINT_WINDOW_TRANSLUCENT;
        m_paintingOffscreenCapture = true;
        effects->drawWindow(renderTarget, viewport, window, mask, Region::infinite(), paintData);
        m_paintingOffscreenCapture = false;

        GLFramebuffer::popFramebuffer();
        offscreenData->isDirty = false;
        offscreenData->needsGuidanceUpdate = true;
        offscreenData->lastCaptureMs = QDateTime::currentMSecsSinceEpoch();
    }

    void AutoHDREffect::paintOffscreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                         EffectWindow *window, int mask, const Region &deviceRegion,
                                         const WindowPaintData &data, const WindowQuadList &quads)
    {
        auto it = m_offscreenWindows.find(window);
        if (it == m_offscreenWindows.end() || !it->second->texture || !m_shader) {
            WindowPaintData fallbackData = data;
            effects->drawWindow(renderTarget, viewport, window, mask, deviceRegion, fallbackData);
            return;
        }

        OffscreenWindowData *offscreenData = it->second.get();
        GLShader *shader = m_shader.get();

        const CalibrationSettings settings = m_activeWindows.value(window, m_globalDefaults);
        if (shouldUseAi(settings)) {
            updateGuidanceMap(window, offscreenData, settings);
        }

        ShaderBinder binder(shader);
        const double scale = viewport.scale();

        GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
        vbo->reset();
        vbo->setAttribLayout(std::span(GLVertexBuffer::GLVertex2DLayout), sizeof(GLVertex2D));

        RenderGeometry geometry;
        for (const WindowQuad &quad : quads) {
            geometry.appendWindowQuad(quad, scale);
        }
        geometry.postProcessTextureCoordinates(offscreenData->texture->matrix(NormalizedCoordinates));

        const auto map = vbo->map<GLVertex2D>(geometry.size());
        if (!map) {
            return;
        }
        geometry.copy(*map);
        vbo->unmap();
        vbo->bindArrays();

        const qreal rgb = data.brightness() * data.opacity();
        const qreal a = data.opacity();

        QMatrix4x4 mvp = viewport.projectionMatrix();
        mvp.translate(std::round(window->x() * scale), std::round(window->y() * scale));

        const auto toXYZ = renderTarget.colorDescription()->containerColorimetry().toXYZ();
        shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp * data.toMatrix(scale));
        shader->setUniform(GLShader::Vec4Uniform::ModulationConstant, QVector4D(rgb, rgb, rgb, a));
        shader->setUniform(GLShader::FloatUniform::Saturation, data.saturation());
        shader->setUniform(GLShader::Vec3Uniform::PrimaryBrightness, QVector3D(toXYZ(1, 0), toXYZ(1, 1), toXYZ(1, 2)));
        shader->setUniform(GLShader::IntUniform::TextureWidth, offscreenData->texture->width());
        shader->setUniform(GLShader::IntUniform::TextureHeight, offscreenData->texture->height());
        shader->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);

        const bool clipping = deviceRegion != Region::infinite();
        const Region clipRegion =
            clipping ? viewport.transform().map(deviceRegion, renderTarget.transformedSize()) : Region::infinite();

        if (clipping) {
            glEnable(GL_SCISSOR_TEST);
        }

        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        GLTexture *guidanceTexture = m_neutralGuidanceTexture.get();
        const bool hasGuidanceMap = shouldUseAi(settings) && offscreenData->guidanceTexture
            && (offscreenData->guidanceValid || offscreenData->guidanceEverUploaded);
        if (hasGuidanceMap) {
            guidanceTexture = offscreenData->guidanceTexture.get();
        } else if (shouldUseAi(settings) && !m_loggedNeutralGuidance) {
            m_loggedNeutralGuidance = true;
            qWarning() << "AutoHDR Effect: AI guidance not ready, using neutral map";
        }
        if (guidanceTexture) {
            glActiveTexture(GL_TEXTURE1);
            guidanceTexture->bind();
            glActiveTexture(GL_TEXTURE0);
        }

        offscreenData->texture->bind();
        vbo->draw(clipRegion, GL_TRIANGLES, 0, geometry.count(), clipping);
        offscreenData->texture->unbind();

        if (guidanceTexture) {
            glActiveTexture(GL_TEXTURE1);
            guidanceTexture->unbind();
            glActiveTexture(GL_TEXTURE0);
        }

        glDisable(GL_BLEND);
        if (clipping) {
            glDisable(GL_SCISSOR_TEST);
        }
        vbo->unbindArrays();
    }

    void AutoHDREffect::paintCompositorMargin(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                              EffectWindow *window, int mask, const Region &deviceRegion,
                                              WindowPaintData &data)
    {
        const RectF expandedGeometry = snapToPixels(window->expandedGeometry(), viewport.scale());
        const RectF frameGeometry = snapToPixels(window->frameGeometry(), viewport.scale());

        const Rect expanded = expandedGeometry.toAlignedRect();
        const Rect frame = frameGeometry.toAlignedRect();
        if (expanded == frame) {
            return;
        }

        Region marginRegion = Region(expanded) - Region(frame);
        if (deviceRegion != Region::infinite()) {
            marginRegion &= deviceRegion;
        }
        if (marginRegion.isEmpty()) {
            return;
        }

        m_paintingCompositorMargin = true;
        effects->drawWindow(renderTarget, viewport, window, mask, marginRegion, data);
        m_paintingCompositorMargin = false;
    }

    bool AutoHDREffect::blocksDirectScanout() const
    {
        return !m_activeWindows.isEmpty();
    }

    void AutoHDREffect::performUnredirect(EffectWindow *window)
    {
        if (!window || !m_offscreenWindows.contains(window)) {
            m_pendingUnredirects.remove(window);
            return;
        }

        unredirect(window);
        m_pendingUnredirects.remove(window);
    }

    bool AutoHDREffect::activateWindow(EffectWindow *window, const CalibrationSettings &settings)
    {
        if (!window || !loadShader()) {
            return false;
        }

        m_pendingUnredirects.remove(window);

        CalibrationSettings sanitized = settings;
        AutoHdr::sanitizeCalibrationSettings(sanitized, m_hdrReferenceNits, m_hdrMaxDisplayNits, m_config);
        m_activeWindows.insert(window, sanitized);

        updateUniforms(sanitized);

        if (!m_offscreenWindows.contains(window)) {
            redirect(window);
        } else {
            auto it = m_offscreenWindows.find(window);
            if (it != m_offscreenWindows.end()) {
                ensureWindowRedirect(window, it->second.get());
            }
        }

        if (isBrowserWindow(window)) {
            applyBrowserSdrPreference(window);
            if (!m_loggedBrowserHdrToast) {
                m_loggedBrowserHdrToast = true;
                showStatusToast(window,
                                QStringLiteral("Zen/Firefox: AutoHDR keeps surface redirect alive when idle; "
                                               "gfx.wayland.hdr off is optional"));
            }
        }

        window->addRepaintFull();
        ensureCompositorHeartbeat();
        return true;
    }

    void AutoHDREffect::scheduleUnredirect(EffectWindow *window)
    {
        if (!window) {
            return;
        }

        m_activeWindows.remove(window);
        ensureCompositorHeartbeat();

        if (!m_offscreenWindows.contains(window)) {
            m_pendingUnredirects.remove(window);
            return;
        }

        invalidateAsyncGuidanceForWindow(window);
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

        if (activateWindow(window, profile->settings)) {
            m_statusToasts[window].wasOnHdrOutput = AutoHdr::windowOnHdrOutput(window);
            showStatusToast(window, enabledStatusMessage(profile->settings));
            qInfo() << "AutoHDR Effect: auto-activated for" << window->windowClass()
                    << "AI"
                    << (shouldUseAi(profile->settings) ? activeGuidanceBackendLabel() : QStringLiteral("off"));
        }
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
            if (m_calibrationOverlay && window == m_calibratingWindow) {
                continue;
            }
            scheduleUnredirect(window);
        }

        for (EffectWindow *window : effects->stackingOrder()) {
            maybeAutoActivateWindow(window);
        }
    }

    void AutoHDREffect::prePaintScreen(ScreenPrePaintData &data)
    {
        if (m_compositorFrameTimer.isValid()) {
            m_lastCompositorFrameMs = m_compositorFrameTimer.elapsed();
        }
        m_frameBudgetSkipOrt = m_lastCompositorFrameMs > 14.0 && m_lastCompositorFrameMs > 0.0;
        finishPendingOrtReadback();
        pollAsyncGuidanceResults();
        scheduleActiveHdrRepaints();
        if (anyActiveHdrWindow()) {
            data.mask |= PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS;
        }
        m_currentPaintOutput = data.screen;
        if (!m_currentPaintOutput && data.view) {
            m_currentPaintOutput = data.view->logicalOutput();
        }
        effects->prePaintScreen(data);
    }

    void AutoHDREffect::postPaintScreen()
    {
        ++m_guidanceFrameCounter;
        if (!m_compositorFrameTimer.isValid()) {
            m_compositorFrameTimer.start();
        } else {
            m_compositorFrameTimer.restart();
        }
        m_currentPaintOutput = nullptr;
        for (auto it = m_activeWindows.constBegin(); it != m_activeWindows.constEnd(); ++it) {
            EffectWindow *window = it.key();
            if (!window || !AutoHdr::windowOnHdrOutput(window)) {
                continue;
            }
            window->addRepaintFull();
            effects->addRepaint(window->expandedGeometry());
        }
        if (!m_activeWindows.isEmpty()) {
            effects->addRepaintFull();
        }
        effects->postPaintScreen();
    }

    void AutoHDREffect::prePaintWindow(RenderView *view, EffectWindow *w, WindowPrePaintData &data)
    {
        if (m_pendingUnredirects.contains(w) && !m_activeWindows.contains(w)) {
            performUnredirect(w);
        }

        if (m_activeWindows.contains(w) && m_shader) {
            updateUniforms(m_activeWindows.value(w));
            if (m_toneCurveLutDirty) {
                uploadToneCurveUniforms();
            }
        }

        if (m_activeWindows.contains(w) && AutoHdr::windowOnHdrOutput(w)) {
            const auto offIt = m_offscreenWindows.find(w);
            if (offIt != m_offscreenWindows.end()) {
                ensureWindowRedirect(w, offIt->second.get());
            }
            data.setTransformed();
            w->addRepaintFull();
            effects->addRepaintFull();
        }

        effects->prePaintWindow(view, w, data);
    }

    void AutoHDREffect::drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                   EffectWindow *window, int mask, const Region &deviceRegion, WindowPaintData &data)
    {
        auto it = m_offscreenWindows.find(window);
        if (it == m_offscreenWindows.end()) {
            logIdlePaintState(window, "no-offscreen");
            effects->drawWindow(renderTarget, viewport, window, mask, deviceRegion, data);
            return;
        }

        if (m_paintingCompositorMargin) {
            effects->drawWindow(renderTarget, viewport, window, mask, deviceRegion, data);
            return;
        }

        if (m_paintingOffscreenCapture) {
            effects->drawWindow(renderTarget, viewport, window, mask, deviceRegion, data);
            return;
        }

        if (m_pendingUnredirects.contains(window)) {
            effects->drawWindow(renderTarget, viewport, window, mask, deviceRegion, data);
            return;
        }

        if (m_activeWindows.contains(window) && m_shader) {
            updateUniforms(m_activeWindows.value(window));
            if (m_toneCurveLutDirty) {
                uploadToneCurveUniforms();
            }
        }

        const bool activeOnHdr =
            m_activeWindows.contains(window) && AutoHdr::windowOnHdrOutput(window);
        if (!activeOnHdr && !AutoHdr::shouldApplyHdrForPaint(effects, m_currentPaintOutput, viewport)) {
            logIdlePaintState(window, "native-bypass");
            if (debugPaintEnabled()) {
                static QSet<EffectWindow *> loggedNativeBypass;
                if (!loggedNativeBypass.contains(window)) {
                    loggedNativeBypass.insert(window);
                    qInfo() << "AutoHDR Effect: native bypass for" << window->windowClass();
                }
            }
            effects->drawWindow(renderTarget, viewport, window, mask, deviceRegion, data);
            return;
        }

        OffscreenWindowData *offscreenData = it->second.get();
        ensureWindowRedirect(window, offscreenData);
        logIdlePaintState(window, "paintOffscreen");

        if (debugPaintEnabled()) {
            static QSet<EffectWindow *> loggedPaintOffscreen;
            if (!loggedPaintOffscreen.contains(window)) {
                loggedPaintOffscreen.insert(window);
                qInfo() << "AutoHDR Effect: paintOffscreen path for" << window->windowClass();
            }
        }

        const RectF frameGeometry = snapToPixels(window->frameGeometry(), viewport.scale());

        RectF visibleRect(0, 0, frameGeometry.width(), frameGeometry.height());
        WindowQuad quad;
        quad[0] = WindowVertex(visibleRect.topLeft(), QPointF(0, 0));
        quad[1] = WindowVertex(visibleRect.topRight(), QPointF(1, 0));
        quad[2] = WindowVertex(visibleRect.bottomRight(), QPointF(1, 1));
        quad[3] = WindowVertex(visibleRect.bottomLeft(), QPointF(0, 1));

        WindowQuadList quads;
        quads.append(quad);

        paintCompositorMargin(renderTarget, viewport, window, mask, deviceRegion, data);
        maybeRenderOffscreen(window);
        paintOffscreen(renderTarget, viewport, window, mask, deviceRegion, data, quads);
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
            showStatusToast(active, QStringLiteral("AutoHDR disabled"));
            qInfo() << "AutoHDR Effect: disabled for" << active->windowClass();
            return;
        }

        if (m_pendingUnredirects.contains(active)) {
            m_pendingUnredirects.remove(active);
            const QString knownKey = resolvedAppKeyForWindow(active);
            const CalibrationSettings settings = knownKey.isEmpty() ? m_globalDefaults : settingsForAppKey(knownKey);
            activateWindow(active, settings);
            m_statusToasts[active].wasOnHdrOutput = AutoHdr::windowOnHdrOutput(active);
            showStatusToast(active, enabledStatusMessage(settings));
            qInfo() << "AutoHDR Effect: re-enabled for" << active->windowClass()
                    << "AI" << (shouldUseAi(settings) ? activeGuidanceBackendLabel() : QStringLiteral("off"));
            return;
        }

        const QString knownKey = resolvedAppKeyForWindow(active);
        const CalibrationSettings settings = knownKey.isEmpty() ? m_globalDefaults : settingsForAppKey(knownKey);
        activateWindow(active, settings);
        m_statusToasts[active].wasOnHdrOutput = AutoHdr::windowOnHdrOutput(active);
        showStatusToast(active, enabledStatusMessage(settings));
        qInfo() << "AutoHDR Effect: enabled for" << active->windowClass()
                << "AI" << (shouldUseAi(settings) ? activeGuidanceBackendLabel() : QStringLiteral("off"));
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
        reevaluateAllWindows();
        if (m_calibrationOverlay) {
            m_calibrationOverlay->setGlobalAiEnabled(m_aiEnabledGlobal && !m_aiForceDisabled);
        }
        qInfo() << "AutoHDR Effect: perceptual color" << (m_perceptualColorEnabled ? "enabled" : "disabled");
        qInfo() << "AutoHDR Effect: AI global" << (m_aiEnabledGlobal && !m_aiForceDisabled)
                << "backend" << activeGuidanceBackendLabel() << "strength" << m_aiStrengthGlobal;
        for (auto it = m_activeWindows.constBegin(); it != m_activeWindows.constEnd(); ++it) {
            if (!it.key()) {
                continue;
            }
            qInfo() << "AutoHDR Effect: AI per-app" << it.key()->windowClass()
                    << "enabled" << it.value().aiEnhanced
                    << "effective" << shouldUseAi(it.value());
        }
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

    void AutoHDREffect::applyCalibrationDraft()
    {
        if (!m_calibratingWindow || !m_calibrationOverlay) {
            return;
        }

        m_calibrationDraft = m_calibrationOverlay->currentValues();
        m_perceptualColorEnabled = m_calibrationOverlay->perceptualColorEnabled();
        AutoHdr::sanitizeCalibrationSettings(m_calibrationDraft, m_hdrReferenceNits, m_hdrMaxDisplayNits, m_config);
        m_calibrationDraftActive = true;
        invalidateAllGuidance();

        if (m_activeWindows.contains(m_calibratingWindow)) {
            m_activeWindows.insert(m_calibratingWindow, m_calibrationDraft);
            activateWindow(m_calibratingWindow, m_calibrationDraft);
        }
        repaintActiveWindows();
    }

    void AutoHDREffect::restoreCalibrationBaseline()
    {
        m_calibrationDraft = m_calibrationBaseline;
        m_calibrationDraftActive = false;
        m_perceptualColorEnabled = m_calibrationPerceptualBaseline;
        reloadActiveWindowSettings();
        repaintActiveWindows();
    }

    void AutoHDREffect::saveCalibrationProfile()
    {
        if (m_calibratingAppKey.isEmpty() || !m_calibrationOverlay) {
            return;
        }

        AutoHdr::AppProfile profile;
        profile.metadata.key = m_calibratingAppKey;
        if (m_calibratingWindow) {
            const WindowIdentifiers ids = identifiersForWindow(m_calibratingWindow);
            profile.metadata.displayName = ids.displayName;
            profile.metadata.windowClass = ids.windowClass;
            profile.metadata.resourceClass = ids.resourceClass;
            profile.metadata.desktopFile = ids.desktopFile;
        } else {
            const std::optional<AutoHdr::AppProfile> existing = AutoHdr::loadAppProfile(m_config, m_calibratingAppKey);
            if (existing) {
                profile.metadata = existing->metadata;
            } else {
                profile.metadata.displayName = m_calibratingAppKey;
            }
        }
        profile.metadata.autoActivate = true;
        profile.settings = m_calibrationOverlay->currentValues();
        AutoHdr::sanitizeCalibrationSettings(profile.settings, m_hdrReferenceNits, m_hdrMaxDisplayNits, m_config);
        AutoHdr::saveAppProfile(m_config, profile);

        AutoHdr::GeneralSettings general = AutoHdr::loadGeneralSettings(m_config);
        general.perceptualColorEnabled = m_calibrationOverlay->perceptualColorEnabled();
        AutoHdr::saveGeneralSettings(m_config, general);
    }

    void AutoHDREffect::applyInternalOverlayPresentation(QWidget *overlay)
    {
        QWindow *handle = overlay ? overlay->windowHandle() : nullptr;
        if (!handle || !Workspace::self()) {
            return;
        }

        Window *window = Workspace::self()->findInternal(handle);
        if (!window) {
            if (!m_warnedOverlayHdrPresentation) {
                qInfo() << "AutoHDR Effect: overlay HDR presentation unavailable; using boosted SDR colors";
                m_warnedOverlayHdrPresentation = true;
            }
            return;
        }

        constexpr float kOverlayPeakFactor = 0.8f;
        const double peak = static_cast<double>(m_hdrMaxDisplayNits) * kOverlayPeakFactor;
        const double avg = peak * 0.5;

        window->setPreferredColorDescription(ColorDescription::BT2020PQ->withHdrMetadata(avg, peak));
    }

    void AutoHDREffect::applyInternalOverlayBlur(QWidget *overlay, const QRect &region)
    {
        QWindow *handle = overlay ? overlay->windowHandle() : nullptr;
        if (!handle) {
            return;
        }

        if (!effects->isEffectLoaded(QStringLiteral("blur"))) {
            if (!m_warnedOverlayBlur) {
                qInfo() << "AutoHDR Effect: overlay blur unavailable; enable the Blur desktop effect";
                m_warnedOverlayBlur = true;
            }
            return;
        }

        if (region.isEmpty()) {
            handle->setProperty("kwin_blur", QVariant());
            return;
        }

        handle->setProperty("kwin_blur", QVariant::fromValue(KWin::RegionF(QRegion(region))));
    }

    void AutoHDREffect::applyOverlayHdrPresentation(CalibrationOverlay *overlay)
    {
        applyInternalOverlayPresentation(overlay);
    }

    void AutoHDREffect::applyOverlayBlur(CalibrationOverlay *overlay)
    {
        applyInternalOverlayBlur(overlay, overlay ? overlay->panelBlurRegion() : QRect());
    }

    void AutoHDREffect::showStatusToast(EffectWindow *window, const QString &message)
    {
        if (!window) {
            return;
        }

        WindowStatusToast &state = m_statusToasts[window];
        if (!state.overlay) {
            state.overlay = new StatusToastOverlay();
            connect(state.overlay.data(), &StatusToastOverlay::expired, this, [this, window]() {
                hideStatusToast(window);
            });
            state.geometryConnection =
                connect(window, &EffectWindow::windowFrameGeometryChanged, this,
                        [this](EffectWindow *w, const RectF &) {
                            syncStatusToastGeometry(w);
                        });
        }

        state.overlay->setMessage(message);
        syncStatusToastGeometry(window);
        if (!state.overlay) {
            return;
        }

        state.overlay->showFor(5000);
        applyInternalOverlayPresentation(state.overlay);
        applyInternalOverlayBlur(state.overlay, state.overlay->panelBlurRegion());
    }

    void AutoHDREffect::syncStatusToastGeometry(EffectWindow *window)
    {
        const auto it = m_statusToasts.find(window);
        if (it == m_statusToasts.end() || !it->overlay) {
            return;
        }

        StatusToastOverlay *overlay = it->overlay.data();
        if (window->isMinimized() || !window->isOnCurrentDesktop()) {
            overlay->hide();
            return;
        }

        const QRect frame = window->frameGeometry().toRect();
        if (!frame.isValid()) {
            return;
        }

        constexpr int inset = 16;
        const QSize pillSize = overlay->size();
        const QPoint topLeft(frame.right() - pillSize.width() - inset + 1, frame.top() + inset);
        overlay->setGeometry(QRect(topLeft, pillSize));

        if (!overlay->isVisible()) {
            overlay->show();
        }
        applyInternalOverlayPresentation(overlay);
        applyInternalOverlayBlur(overlay, overlay->panelBlurRegion());
    }

    void AutoHDREffect::hideStatusToast(EffectWindow *window)
    {
        const auto it = m_statusToasts.find(window);
        if (it == m_statusToasts.end()) {
            return;
        }

        if (it->geometryConnection) {
            disconnect(it->geometryConnection);
            it->geometryConnection = {};
        }

        if (StatusToastOverlay *overlay = it->overlay.data()) {
            if (QWindow *handle = overlay->windowHandle()) {
                handle->setProperty("kwin_blur", QVariant());
            }
            overlay->hide();
            overlay->deleteLater();
        }

        it->overlay = nullptr;
    }

    void AutoHDREffect::removeStatusToast(EffectWindow *window)
    {
        hideStatusToast(window);
        m_statusToasts.remove(window);
    }

    void AutoHDREffect::syncCalibrationOverlayGeometry(EffectWindow *window)
    {
        if (!m_calibrationOverlay || !window || window != m_calibratingWindow) {
            return;
        }

        if (window->isMinimized() || !window->isOnCurrentDesktop()) {
            m_calibrationOverlay->hide();
            return;
        }

        const QRect geometry = window->frameGeometry().toRect();
        if (!geometry.isValid()) {
            return;
        }

        m_calibrationOverlay->setGeometry(geometry);
        if (!m_calibrationOverlay->isVisible()) {
            m_calibrationOverlay->show();
            m_calibrationOverlay->raise();
            m_calibrationOverlay->activateWindow();
        }
        applyOverlayHdrPresentation(m_calibrationOverlay);
        applyOverlayBlur(m_calibrationOverlay);
    }

    void AutoHDREffect::closeCalibrationOverlay(bool saved)
    {
        EffectWindow *calibratingWindow = m_calibratingWindow;

        if (m_frameGeometryConnection) {
            disconnect(m_frameGeometryConnection);
            m_frameGeometryConnection = QMetaObject::Connection();
        }

        if (m_calibrationOverlay) {
            if (QWindow *handle = m_calibrationOverlay->windowHandle()) {
                handle->setProperty("kwin_blur", QVariant());
            }
            m_calibrationOverlay->hide();
            m_calibrationOverlay->deleteLater();
            m_calibrationOverlay = nullptr;
        }

        invalidateAsyncGuidanceForWindow(calibratingWindow);

        if (!saved && m_calibrationDraftActive) {
            restoreCalibrationBaseline();
        } else if (saved) {
            m_calibrationDraftActive = false;
            reloadSettings();
            showTransientOnScreenMessage(QStringLiteral("AutoHDR calibration confirmed"),
                                         QStringLiteral("video-display"));
        } else {
            m_calibrationDraftActive = false;
        }

        m_calibratingWindow = nullptr;
        m_calibratingAppKey.clear();
        effects->hideOnScreenMessage();
    }

    void AutoHDREffect::openCalibrationOverlay()
    {
        if (!m_calibratingWindow || m_calibratingAppKey.isEmpty()) {
            return;
        }

        if (m_calibratingWindow->isMinimized()) {
            showTransientOnScreenMessage(QStringLiteral("Restore the window before calibrating AutoHDR"),
                                         QStringLiteral("dialog-warning"));
            m_calibratingWindow = nullptr;
            m_calibratingAppKey.clear();
            return;
        }

        reloadHdrDisplayLimits();

        m_calibrationBaseline = settingsForAppKey(m_calibratingAppKey);
        m_calibrationDraft = m_calibrationBaseline;
        m_calibrationDraftActive = true;
        m_calibrationPerceptualBaseline = m_perceptualColorEnabled;

        auto *overlay = new CalibrationOverlay();
        m_calibrationOverlay = overlay;

        overlay->setConfig(m_config);
        overlay->setHdrLimits(qRound(m_hdrReferenceNits) + 1, qRound(m_hdrMaxDisplayNits));
        overlay->setValues(m_calibrationDraft);
        overlay->setPerceptualColorEnabled(m_perceptualColorEnabled);
        overlay->setGlobalAiEnabled(m_aiEnabledGlobal && !m_aiForceDisabled);

        connect(overlay, &CalibrationOverlay::settingsChanged, this, &AutoHDREffect::applyCalibrationDraft);
        connect(overlay, &CalibrationOverlay::settingsCommitted, this, &AutoHDREffect::applyCalibrationDraft);
        connect(overlay, &CalibrationOverlay::confirmed, this, [this]() {
            saveCalibrationProfile();
            closeCalibrationOverlay(true);
        });
        connect(overlay, &CalibrationOverlay::cancelled, this, [this]() {
            closeCalibrationOverlay(false);
        });

        m_frameGeometryConnection = connect(m_calibratingWindow, &EffectWindow::windowFrameGeometryChanged, this,
                                            [this](EffectWindow *window, const RectF &) {
                                                syncCalibrationOverlayGeometry(window);
                                            });

        syncCalibrationOverlayGeometry(m_calibratingWindow);
        overlay->show();
        overlay->raise();
        overlay->activateWindow();
        overlay->setFocus();
        QTimer::singleShot(0, this, [this, overlay]() {
            if (m_calibrationOverlay == overlay) {
                applyOverlayHdrPresentation(overlay);
                applyOverlayBlur(overlay);
            }
        });

        if (!m_activeWindows.contains(m_calibratingWindow)) {
            activateWindow(m_calibratingWindow, m_calibrationDraft);
        } else {
            applyCalibrationDraft();
        }
    }

    void AutoHDREffect::toggleOverlay()
    {
        if (m_calibrationOverlay) {
            closeCalibrationOverlay(false);
            return;
        }

        EffectWindow *active = effects->activeWindow();
        if (!active) {
            showTransientOnScreenMessage(QStringLiteral("Select a window to calibrate AutoHDR"),
                                         QStringLiteral("dialog-warning"));
            return;
        }

        if (!isEligibleWindow(active)) {
            showTransientOnScreenMessage(QStringLiteral("This window type cannot be calibrated"),
                                         QStringLiteral("dialog-warning"));
            return;
        }

        m_calibratingWindow = active;
        m_calibratingAppKey = appKeyForWindow(active);

        if (!m_calibratingAppKey.isEmpty()) {
            const QString knownKey = findKnownAppKey(active);
            if (!knownKey.isEmpty()) {
                m_calibratingAppKey = knownKey;
            }
        }

        if (m_calibratingAppKey.isEmpty()) {
            showTransientOnScreenMessage(QStringLiteral("Could not identify the active application"),
                                         QStringLiteral("dialog-warning"));
            m_calibratingWindow = nullptr;
            return;
        }

        qInfo() << "AutoHDR Effect: opening calibration overlay for" << active->windowClass()
                << "app key" << m_calibratingAppKey;
        showTransientOnScreenMessage(QStringLiteral("Opening AutoHDR calibration…"), QStringLiteral("configure"));
        QMetaObject::invokeMethod(this, &AutoHDREffect::openCalibrationOverlay, Qt::QueuedConnection);
    }

} // namespace KWin

#include "autohdr_effect.moc"
