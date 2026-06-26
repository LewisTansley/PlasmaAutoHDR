#pragma once

#include <autohdr/autohdr.h>

#include <KSharedConfig>
#include <QPointF>
#include <QString>
#include <QVector>

#include "autohdr_config.h"
#include "tone_curve_user_presets.h"

namespace AutoHdr {

QVector<QPointF> toQVector(const AutoHdrCore::PointList &points);
AutoHdrCore::PointList toPointList(const QVector<QPointF> &points);
AutoHdrCore::Vec2 toVec2(const QPointF &point);
QPointF toQPointF(const AutoHdrCore::Vec2 &point);

AutoHdrCore::CalibrationSettings toCore(const CalibrationSettings &qtSettings);
CalibrationSettings fromCore(const AutoHdrCore::CalibrationSettings &coreSettings);
AutoHdrCore::ToneCurvePreset toCore(ToneCurvePreset preset);
ToneCurvePreset fromCore(AutoHdrCore::ToneCurvePreset preset);

std::vector<AutoHdrCore::UserToneCurvePreset> loadCoreUserPresets(const KSharedConfigPtr &config);

} // namespace AutoHdr
