#include "tone_curve.h"

#include "autohdr_qt_bridge.h"

namespace AutoHdr {

float clampReferenceNits(float value)
{
    return AutoHdrCore::clampReferenceNits(value);
}

float clampBlackPoint(float value)
{
    return AutoHdrCore::clampBlackPoint(value);
}

float clampVibrance(float value)
{
    return AutoHdrCore::clampVibrance(value);
}

float clampGamutExpansion(float value)
{
    return AutoHdrCore::clampGamutExpansion(value);
}

QVector<QPointF> buildFullCurve(const ToneCurveEndpoints &endpoints, const QVector<QPointF> &intermediate)
{
    AutoHdrCore::ToneCurveEndpoints coreEndpoints;
    coreEndpoints.peakNits = endpoints.peakNits;
    coreEndpoints.sdrMaxPoint = toVec2(endpoints.sdrMaxPoint);
    coreEndpoints.visualReferenceNits = endpoints.visualReferenceNits;
    return toQVector(AutoHdrCore::buildFullCurve(coreEndpoints, toPointList(intermediate)));
}

QVector<QPointF> sanitizeIntermediatePoints(const QVector<QPointF> &points, const ToneCurveEndpoints &endpoints)
{
    AutoHdrCore::ToneCurveEndpoints coreEndpoints;
    coreEndpoints.peakNits = endpoints.peakNits;
    coreEndpoints.sdrMaxPoint = toVec2(endpoints.sdrMaxPoint);
    coreEndpoints.visualReferenceNits = endpoints.visualReferenceNits;
    return toQVector(AutoHdrCore::sanitizeIntermediatePoints(toPointList(points), coreEndpoints));
}

QPointF sanitizeSdrMaxPoint(const QPointF &point, const ToneCurveEndpoints &endpoints,
                            const QVector<QPointF> &intermediate)
{
    AutoHdrCore::ToneCurveEndpoints coreEndpoints;
    coreEndpoints.peakNits = endpoints.peakNits;
    coreEndpoints.sdrMaxPoint = toVec2(endpoints.sdrMaxPoint);
    coreEndpoints.visualReferenceNits = endpoints.visualReferenceNits;
    return toQPointF(
        AutoHdrCore::sanitizeSdrMaxPoint(toVec2(point), coreEndpoints, toPointList(intermediate)));
}

float evaluateToneCurve(const QVector<QPointF> &fullCurve, float inputNits)
{
    return AutoHdrCore::evaluateToneCurve(toPointList(fullCurve), inputNits);
}

void buildToneCurveLut(const QVector<QPointF> &fullCurve, float inputSpan, float *lut, int size)
{
    AutoHdrCore::buildToneCurveLut(toPointList(fullCurve), inputSpan, lut, size);
}

QString formatToneCurvePoints(const QVector<QPointF> &points)
{
    return QString::fromStdString(AutoHdrCore::formatToneCurvePoints(toPointList(points)));
}

QVector<QPointF> parseToneCurvePoints(const QString &encoded)
{
    return toQVector(AutoHdrCore::parseToneCurvePoints(encoded.toStdString()));
}

QString formatSdrMaxPoint(const QPointF &point)
{
    return QString::fromStdString(AutoHdrCore::formatSdrMaxPoint(toVec2(point)));
}

QPointF parseSdrMaxPoint(const QString &encoded, const QPointF &fallback)
{
    return toQPointF(AutoHdrCore::parseSdrMaxPoint(encoded.toStdString(), toVec2(fallback)));
}

} // namespace AutoHdr
