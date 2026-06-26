#pragma once

#include <KSharedConfig>
#include <QPointF>
#include <QString>
#include <QVector>

namespace AutoHdr {

struct CalibrationSettings;

enum class ToneCurvePreset {
    Custom,
    Linear,
    SCurve,
    SCurveBoosted,
    SCurveLifted,
    Exponential,
    User,
};

struct PresetCurveParams {
    float referenceNits = 203.0f;
    float peakNits = 1000.0f;
};

QString presetToString(ToneCurvePreset preset);
ToneCurvePreset presetFromString(const QString &encoded);

QVector<ToneCurvePreset> builtInToneCurvePresets();
QString presetDisplayName(ToneCurvePreset preset);

QVector<QPointF> generatePresetIntermediatePoints(ToneCurvePreset preset, const PresetCurveParams &params);
void applyToneCurvePreset(CalibrationSettings &settings, const KSharedConfigPtr &config = KSharedConfigPtr());
ToneCurvePreset detectMatchingPreset(const CalibrationSettings &settings, float toleranceNits = 2.0f,
                                     const KSharedConfigPtr &config = KSharedConfigPtr());

} // namespace AutoHdr
