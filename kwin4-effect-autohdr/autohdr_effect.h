#pragma once
#include "autohdr_config.h"
#include "tone_curve.h"
#include <effect/offscreeneffect.h>
#include <KSharedConfig>
#include <QDateTime>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <memory>

class QAction;
class QProcess;
class QProcessEnvironment;

namespace KWin {

    class GLShader;
    class EffectWindow;
    class RenderView;
    struct WindowPrePaintData;

    class AutoHDREffect : public OffscreenEffect {
        Q_OBJECT
        Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.effect.autohdr")

    public:
        using CalibrationSettings = AutoHdr::CalibrationSettings;

        AutoHDREffect();
        ~AutoHDREffect() override;

        bool isActive() const override;
        void reconfigure(ReconfigureFlags flags) override;

    public Q_SLOTS:
        void toggleAutoHDR();
        void toggleOverlay();
        Q_SCRIPTABLE void reloadSettings();

    protected:
        void prePaintWindow(RenderView *view, EffectWindow *w, WindowPrePaintData &data,
                            std::chrono::milliseconds presentTime) override;
        void drawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindow *window, int mask,
                        const Region &deviceRegion, WindowPaintData &data) override;

    private:
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
        void activateWindow(EffectWindow *window, const CalibrationSettings &settings);
        void scheduleUnredirect(EffectWindow *window);
        void performUnredirect(EffectWindow *window);
        void runCalibrationDialog();
        void finishCalibration(bool saved, bool usedPythonScript = false);
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
        QSet<EffectWindow *> m_redirectedWindows;
        QSet<EffectWindow *> m_pendingUnredirects;
        std::unique_ptr<GLShader> m_shader;
        KSharedConfigPtr m_config;
        QPointer<QProcess> m_kdialogProcess;
        int m_calibrationStep = 0;
        bool m_dbusRegistered = false;
        bool m_autoActivateCalibrated = true;
        QString m_calibratingAppKey;
        EffectWindow *m_calibratingWindow = nullptr;

        CalibrationSettings m_globalDefaults;
        float m_hdrReferenceNits = 100.0f;
        float m_hdrMaxDisplayNits = 1000.0f;

        int m_locMaxNits = -1;
        int m_locGamutExpansion = -1;
        int m_locBlackPoint = -1;
        int m_locMidPoint = -1;
        int m_locHighlightExpansion = -1;
        int m_locHighlightLift = -1;
        int m_locHighlightRange = -1;
        int m_locColorVibrance = -1;
        int m_locUseToneCurve = -1;
        int m_locToneCurveInputSpan = -1;
        int m_locToneCurveLut = -1;

        float m_toneCurveLut[AutoHdr::kToneCurveLutSize] = {};
        bool m_toneCurveLutDirty = true;
        bool m_cachedUseToneCurve = false;
        float m_cachedToneCurveInputSpan = 203.0f;
        bool m_warnedMissingToneCurveUniforms = false;

        QString m_shaderPath;
        QDateTime m_shaderFragMtime;
    };

} // namespace KWin
