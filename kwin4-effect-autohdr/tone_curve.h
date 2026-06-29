#pragma once

#include <QPointF>
#include <QString>
#include <QVector>

namespace AutoHdr {

constexpr int kToneCurveLutSize = 512;

struct ToneCurveEndpoints {
    float peakNits = 1000.0f;
    QPointF sdrMaxPoint;
    float visualReferenceNits = 100.0f;
};

QVector<QPointF> buildFullCurve(const ToneCurveEndpoints &endpoints, const QVector<QPointF> &intermediate);

QVector<QPointF> sanitizeIntermediatePoints(const QVector<QPointF> &points, const ToneCurveEndpoints &endpoints);

QPointF sanitizeSdrMaxPoint(const QPointF &point, const ToneCurveEndpoints &endpoints,
                            const QVector<QPointF> &intermediate);

float evaluateToneCurve(const QVector<QPointF> &fullCurve, float inputNits);

void buildToneCurveLut(const QVector<QPointF> &fullCurve, float inputSpan, float referenceNits, float *lut, int size);

QString formatToneCurvePoints(const QVector<QPointF> &points);

QVector<QPointF> parseToneCurvePoints(const QString &encoded);

QString formatSdrMaxPoint(const QPointF &point);

QPointF parseSdrMaxPoint(const QString &encoded, const QPointF &fallback);

} // namespace AutoHdr
