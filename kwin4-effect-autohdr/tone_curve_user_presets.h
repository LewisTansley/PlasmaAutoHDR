#pragma once

#include "autohdr_config.h"

#include <KSharedConfig>
#include <QPointF>
#include <QString>
#include <QVector>
#include <optional>

namespace AutoHdr {

struct UserToneCurvePreset {
    QString id;
    QString displayName;
    QVector<QPointF> normalizedPoints;
    QPointF sdrMaxFraction = QPointF(1.0, 1.0);
};

QString userPresetComboId(const QString &id);
bool isUserPresetComboId(const QString &comboId);
QString userPresetIdFromComboId(const QString &comboId);

QString sanitizeUserPresetKey(const QString &raw);
QString userPresetGroupName(const QString &id);

QVector<UserToneCurvePreset> loadUserToneCurvePresets(const KSharedConfigPtr &config);
std::optional<UserToneCurvePreset> loadUserToneCurvePreset(const KSharedConfigPtr &config, const QString &id);
UserToneCurvePreset normalizeCurrentCurve(const CalibrationSettings &settings);
void applyUserToneCurvePreset(CalibrationSettings &settings, const UserToneCurvePreset &preset);
bool saveUserToneCurvePreset(const KSharedConfigPtr &config, const UserToneCurvePreset &preset);
bool deleteUserToneCurvePreset(const KSharedConfigPtr &config, const QString &id);

QString formatNormalizedPoints(const QVector<QPointF> &points);
QVector<QPointF> parseNormalizedPoints(const QString &encoded);
QString formatSdrMaxFraction(const QPointF &fraction);
QPointF parseSdrMaxFraction(const QString &encoded, const QPointF &fallback = QPointF(1.0, 1.0));

} // namespace AutoHdr
