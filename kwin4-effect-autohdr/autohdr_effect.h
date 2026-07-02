#pragma once
#include "autohdr_config.h"
#include "calibration_overlay.h"
#include "status_toast_overlay.h"
#include "tone_curve.h"
#include <effect/effect.h>
#include <scene/item.h>
#include <KSharedConfig>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <map>
#include <memory>

class QAction;

namespace KWin {

    class GLFramebuffer;
    class GLShader;
    class GLTexture;
    class EffectWindow;
    class LogicalOutput;
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
        void prePaintScreen(ScreenPrePaintData &data) override;
        void postPaintScreen() override;
        void prePaintWindow(RenderView *view, EffectWindow *w, WindowPrePaintData &data) override;
        void drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window, int mask,
                        const Region &deviceRegion, WindowPaintData &data) override;

    private:
        struct OffscreenWindowData {
            std::unique_ptr<GLTexture> texture;
            std::unique_ptr<GLFramebuffer> fbo;
            bool isDirty = true;
            QMetaObject::Connection windowDamagedConnection;
            QMetaObject::Connection windowExpandedGeometryConnection;
            ItemEffect windowEffect;
        };

        struct WindowIdentifiers {
            QString desktopFile;
            QString resourceClass;
            QString windowClass;
            QString displayName;
        };

        struct WindowStatusToast {
            QPointer<StatusToastOverlay> overlay;
            QMetaObject::Connection geometryConnection;
            bool wasOnHdrOutput = false;
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
        void paintCompositorMargin(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window,
                                   int mask, const Region &deviceRegion, WindowPaintData &data);
        void handleWindowDeleted(EffectWindow *window);
        void setupOffscreenConnections();
        void destroyOffscreenConnections();
        GLenum redirectInternalFormat() const;
        void openCalibrationOverlay();
        void closeCalibrationOverlay(bool saved);
        void syncCalibrationOverlayGeometry(EffectWindow *window);
        void applyCalibrationDraft();
        void restoreCalibrationBaseline();
        void saveCalibrationProfile();
        void applyInternalOverlayPresentation(QWidget *overlay);
        void applyInternalOverlayBlur(QWidget *overlay, const QRect &region);
        void applyOverlayHdrPresentation(CalibrationOverlay *overlay);
        void applyOverlayBlur(CalibrationOverlay *overlay);
        void showStatusToast(EffectWindow *window, const QString &message);
        void syncStatusToastGeometry(EffectWindow *window);
        void hideStatusToast(EffectWindow *window);
        void removeStatusToast(EffectWindow *window);
        void onWindowOutputChanged(EffectWindow *window);
        void showTransientOnScreenMessage(const QString &message, const QString &iconName = QString());
        void repaintActiveWindows();
        void reloadHdrDisplayLimits();
        void sanitizeGlobalDefaults(bool persist = false);
        void registerDBusService();
        void unregisterDBusService();
        void registerEffectShortcut(QAction *action, const QString &friendlyName, const QKeySequence &defaultShortcut);
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
        void warnMissingPerceptualUniformsOnce();
        void connectOutputTracking();
        void disconnectOutputTracking();
        void rebindOutputHdrConnections();
        void connectWindowOutputTracking(EffectWindow *window);
        void disconnectWindowOutputTracking(EffectWindow *window);
        void onOutputConfigurationChanged();

        QAction *m_toggleAction = nullptr;
        QAction *m_overlayAction = nullptr;
        QHash<EffectWindow *, CalibrationSettings> m_activeWindows;
        std::map<EffectWindow *, std::unique_ptr<OffscreenWindowData>> m_offscreenWindows;
        QSet<EffectWindow *> m_pendingUnredirects;
        std::unique_ptr<GLShader> m_shader;
        KSharedConfigPtr m_config;
        QPointer<CalibrationOverlay> m_calibrationOverlay;
        QHash<EffectWindow *, WindowStatusToast> m_statusToasts;
        bool m_dbusRegistered = false;
        bool m_autoActivateCalibrated = true;
        bool m_perceptualColorEnabled = true;
        QString m_calibratingAppKey;
        EffectWindow *m_calibratingWindow = nullptr;
        CalibrationSettings m_calibrationBaseline;
        CalibrationSettings m_calibrationDraft;
        bool m_calibrationDraftActive = false;
        bool m_calibrationPerceptualBaseline = true;
        bool m_warnedOverlayHdrPresentation = false;
        bool m_warnedOverlayBlur = false;
        QMetaObject::Connection m_windowDeletedConnection;
        QMetaObject::Connection m_frameGeometryConnection;
        QList<QMetaObject::Connection> m_outputHdrConnections;
        QHash<EffectWindow *, QMetaObject::Connection> m_windowOutputConnections;
        QList<QMetaObject::Connection> m_screenListConnections;
        bool m_outputTrackingConnected = false;
        LogicalOutput *m_currentPaintOutput = nullptr;
        bool m_paintingCompositorMargin = false;

        CalibrationSettings m_globalDefaults;
        float m_hdrReferenceNits = 100.0f;
        float m_hdrMaxDisplayNits = 1000.0f;

        int m_locGamutExpansion = -1;
        int m_locBlackPoint = -1;
        int m_locColorIntensity = -1;
        int m_locPqBoostParams = -1;
        int m_locPerceptualColorEnabled = -1;
        int m_locToneCurveInputSpan = -1;
        int m_locToneCurveLut = -1;
        int m_locDebandStrength = -1;
        int m_locDitherStrength = -1;
        int m_locCurveAntialiasStrength = -1;
        int m_locHighlightSoftness = -1;
        int m_locToneCurveSlopeLut = -1;
        int m_locToneCurveMaxSlope = -1;
        int m_locProcessingQuality = -1;
        int m_locAntiAliasingQuality = -1;
        int m_locEnableSpatialAvgPreCurve = -1;

        float m_toneCurveLut[AutoHdr::kToneCurveLutSize] = {};
        float m_toneCurveSlopeLut[AutoHdr::kToneCurveLutSize] = {};
        float m_toneCurveMaxSlope = 1.0f;
        bool m_toneCurveLutDirty = true;
        float m_cachedToneCurveInputSpan = 203.0f;
        bool m_warnedMissingToneCurveUniforms = false;
        bool m_warnedMissingPerceptualUniforms = false;

        mutable GLenum m_redirectInternalFormat = 0;
        int m_processingQuality = 1;
        float m_debandStrength = 0.25f;
        float m_ditherStrength = 0.05f / 255.0f;
        float m_curveAntialiasStrength = 0.45f;
        float m_highlightSoftness = 0.30f;
        int m_antiAliasingQuality = 0;

        QString m_shaderPath;
        QDateTime m_shaderFragMtime;
        QDateTime m_shaderColorMtime;
        QDateTime m_shaderPerceptualMtime;
    };

} // namespace KWin
