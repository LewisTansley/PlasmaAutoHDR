#include "autohdr_qt_bridge.h"

#include "tone_curve_user_presets.h"

namespace AutoHdr {

QVector<QPointF> toQVector(const AutoHdrCore::PointList &points)
{
    QVector<QPointF> result;
    result.reserve(static_cast<int>(points.size()));
    for (const AutoHdrCore::Vec2 &point : points) {
        result.append(toQPointF(point));
    }
    return result;
}

AutoHdrCore::PointList toPointList(const QVector<QPointF> &points)
{
    AutoHdrCore::PointList result;
    result.reserve(points.size());
    for (const QPointF &point : points) {
        result.push_back(toVec2(point));
    }
    return result;
}

AutoHdrCore::Vec2 toVec2(const QPointF &point)
{
    return {static_cast<float>(point.x()), static_cast<float>(point.y())};
}

QPointF toQPointF(const AutoHdrCore::Vec2 &point)
{
    return QPointF(point.x, point.y);
}

AutoHdrCore::CalibrationSettings toCore(const CalibrationSettings &qtSettings)
{
    AutoHdrCore::CalibrationSettings core;
    core.maxNits = qtSettings.maxNits;
    core.gamutExpansion = qtSettings.gamutExpansion;
    core.blackPoint = qtSettings.blackPoint;
    core.vibrance = qtSettings.vibrance;
    core.referenceNits = qtSettings.referenceNits;
    core.sdrMaxPoint = toVec2(qtSettings.sdrMaxPoint);
    core.toneCurvePoints = toPointList(qtSettings.toneCurvePoints);
    core.toneCurvePreset = toCore(qtSettings.toneCurvePreset);
    core.toneCurveUserPresetId = qtSettings.toneCurveUserPresetId.toStdString();
    return core;
}

CalibrationSettings fromCore(const AutoHdrCore::CalibrationSettings &coreSettings)
{
    CalibrationSettings qt;
    qt.maxNits = coreSettings.maxNits;
    qt.gamutExpansion = coreSettings.gamutExpansion;
    qt.blackPoint = coreSettings.blackPoint;
    qt.vibrance = coreSettings.vibrance;
    qt.referenceNits = coreSettings.referenceNits;
    qt.sdrMaxPoint = toQPointF(coreSettings.sdrMaxPoint);
    qt.toneCurvePoints = toQVector(coreSettings.toneCurvePoints);
    qt.toneCurvePreset = fromCore(coreSettings.toneCurvePreset);
    qt.toneCurveUserPresetId = QString::fromStdString(coreSettings.toneCurveUserPresetId);
    return qt;
}

AutoHdrCore::ToneCurvePreset toCore(ToneCurvePreset preset)
{
    return static_cast<AutoHdrCore::ToneCurvePreset>(preset);
}

ToneCurvePreset fromCore(AutoHdrCore::ToneCurvePreset preset)
{
    return static_cast<ToneCurvePreset>(preset);
}

std::vector<AutoHdrCore::UserToneCurvePreset> loadCoreUserPresets(const KSharedConfigPtr &config)
{
    std::vector<AutoHdrCore::UserToneCurvePreset> presets;
    for (const UserToneCurvePreset &preset : loadUserToneCurvePresets(config)) {
        AutoHdrCore::UserToneCurvePreset core;
        core.id = preset.id.toStdString();
        core.displayName = preset.displayName.toStdString();
        core.normalizedPoints = toPointList(preset.normalizedPoints);
        core.sdrMaxFraction = toVec2(preset.sdrMaxFraction);
        presets.push_back(std::move(core));
    }
    return presets;
}

} // namespace AutoHdr
