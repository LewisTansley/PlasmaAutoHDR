#pragma once
#include "autohdr_config.h"
#include "tone_curve.h"
#include <effect/effect.h>
#include <scene/item.h>
#include <KSharedConfig>
#include <QDateTime>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <map>
#include <memory>

class QAction;
class QProcess;
class QProcessEnvironment;

namespace KWin {

    class GLFramebuffer;
    class GLShader;
    class GLTexture;
    class EffectWindow;
    class RenderView;
    struct WindowPrePaintData;

    class AutoHDREffect : public Effect {
        Q_OBJECT
        Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.effect.autohdr")

    public:
        using CalibrationSettings = AutoHdr::CalibrationSettings;

        AutoHDREffect();
        ~AutoHDREffect() override;

        bool isActive() const override;
        bool blocksDirectScanout() const override;
        void reconfigure(ReconfigureFlags flags) override;

    public Q_SLOTS:
        void toggleAutoHDR();
        void toggleOverlay();
        Q_SCRIPTABLE void reloadSettings();

    protected:
        void prePaintWindow(RenderView *view, EffectWindow *w, WindowPrePaintData &data) override;
        void drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window, int mask,
                        const Region &deviceRegion, WindowPaintData &data) override;

    private:
        struct OffscreenWindowData {
            std::unique_ptr<GLTexture> texture;
            std::unique_ptr<GLFramebuffer> fbo;
            bool isDirty = true;
            QMetaObject::Connection windowDamagedConnection;
            ItemEffect windowEffect;
        };

        struct WindowIdentifiers {
            QString desktopFile;
            QString resourceClass;
            QString windowClass;
            QString displayName;
        };

        void loadGlobalDefaults(bool persistSanitize = true);
        void saveGlobalDefaults();
        void resolveUniformLocations();
        void updateUniforms(const CalibrationSettings &settings);
        bool loadShader();
        QByteArray loadShaderSource() const;
        bool activateWindow(EffectWindow *window, const CalibrationSettings &settings);
        void scheduleUnredirect(EffectWindow *window);
        void performUnredirect(EffectWindow *window);
        void redirect(EffectWindow *window);
        void unredirect(EffectWindow *window);
        void maybeRenderOffscreen(EffectWindow *window);
        void paintOffscreen(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window,
                            int mask, const Region &deviceRegion, const WindowPaintData &data, const WindowQuadList &quads);
        void handleWindowDeleted(EffectWindow *window);
        void setupOffscreenConnections();
        void destroyOffscreenConnections();
        GLenum redirectInternalFormat() const;
        void runCalibrationDialog();
        void finishCalibration(bool saved);
        void showTransientOnScreenMessage(const QString &message, const QString &iconName = QString());
        void repaintActiveWindows();
        void reloadHdrDisplayLimits();
        void sanitizeGlobalDefaults(bool persist = false);
        void registerDBusService();
        void unregisterDBusService();
        void registerEffectShortcut(QAction *action, const QString &friendlyName, const QKeySequence &defaultShortcut);
        QString calibrationScriptPath() const;
        QProcessEnvironment calibrationProcessEnvironment() const;
        void maybeAutoActivateWindow(EffectWindow *window);
        void reevaluateAllWindows();
        CalibrationSettings settingsForWindow(EffectWindow *window) const;
        CalibrationSettings settingsForAppKey(const QString &appKey) const;
        QString appKeyForWindow(EffectWindow *window) const;
        WindowIdentifiers identifiersForWindow(EffectWindow *window) const;
        QString findKnownAppKey(EffectWindow *window) const;
        bool isEligibleWindow(EffectWindow *window) const;
        void reloadActiveWindowSettings();
        QString resolvedAppKeyForWindow(EffectWindow *window) const;
        void computeToneCurveLut(const CalibrationSettings &settings);
        void uploadToneCurveUniforms();
        void warnMissingToneCurveUniformsOnce();

        QAction *m_toggleAction = nullptr;
        QAction *m_overlayAction = nullptr;
        QHash<EffectWindow *, CalibrationSettings> m_activeWindows;
        std::map<EffectWindow *, std::unique_ptr<OffscreenWindowData>> m_offscreenWindows;
        QSet<EffectWindow *> m_pendingUnredirects;
        std::unique_ptr<GLShader> m_shader;
        KSharedConfigPtr m_config;
        QPointer<QProcess> m_kdialogProcess;
        bool m_dbusRegistered = false;
        bool m_autoActivateCalibrated = true;
        QString m_calibratingAppKey;
        EffectWindow *m_calibratingWindow = nullptr;
        QMetaObject::Connection m_windowDeletedConnection;

        CalibrationSettings m_globalDefaults;
        float m_hdrReferenceNits = 100.0f;
        float m_hdrMaxDisplayNits = 1000.0f;
        float m_hdrMinDisplayNits = 0.0f;

        int m_locGamutExpansion = -1;
        int m_locChromaCompensation = -1;
        int m_locBlackPoint = -1;
        int m_locColorVibrance = -1;
        int m_locToneCurveInputSpan = -1;
        int m_locToneCurveReferenceNits = -1;
        int m_locMinDisplayNits = -1;
        int m_locToneCurveLut = -1;
        int m_locDebandStrength = -1;
        int m_locDitherStrength = -1;
        int m_locProcessingQuality = -1;

        float m_toneCurveLut[AutoHdr::kToneCurveLutSize] = {};
        bool m_toneCurveLutDirty = true;
        float m_cachedToneCurveInputSpan = 203.0f;
        float m_cachedToneCurveReferenceNits = 203.0f;
        bool m_warnedMissingToneCurveUniforms = false;

        mutable GLenum m_redirectInternalFormat = 0;
        int m_processingQuality = 0;
        float m_debandStrength = 0.25f;
        float m_ditherStrength = 0.15f / 255.0f;

        QString m_shaderPath;
        QDateTime m_shaderFragMtime;
        QDateTime m_shaderColorMtime;
    };

} // namespace KWin
