#include "autohdr_config.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace AutoHdr {

namespace {

constexpr float kEpsilon = 1e-6f;
constexpr float kSameXTolerance = 1.0f;

float clampf(float value, float minValue, float maxValue)
{
    return qBound(minValue, value, maxValue);
}

float stepWidthForReference(float referenceNits)
{
    return qMax(1.0f, referenceNits * 0.005f);
}

struct SegmentSlopes {
    QVector<float> m;
};

SegmentSlopes computeMonotonicSlopes(const QVector<QPointF> &points)
{
    const int n = points.size();
    SegmentSlopes result;
    result.m.resize(n);

    if (n < 2) {
        if (n == 1) {
            result.m[0] = 0.0f;
        }
        return result;
    }

    QVector<float> h(n - 1);
    QVector<float> delta(n - 1);
    for (int i = 0; i < n - 1; ++i) {
        h[i] = qMax(static_cast<float>(points[i + 1].x() - points[i].x()), kEpsilon);
        delta[i] = (static_cast<float>(points[i + 1].y()) - static_cast<float>(points[i].y())) / h[i];
    }

    result.m[0] = delta[0];
    result.m[n - 1] = delta[n - 2];

    for (int i = 1; i < n - 1; ++i) {
        if (delta[i - 1] * delta[i] <= 0.0f) {
            result.m[i] = 0.0f;
        } else {
            const float w1 = 2.0f * h[i] + h[i - 1];
            const float w2 = h[i] + 2.0f * h[i - 1];
            result.m[i] = (w1 + w2) / (w1 / delta[i - 1] + w2 / delta[i]);
        }
    }

    for (int i = 0; i < n - 1; ++i) {
        if (std::abs(delta[i]) < kEpsilon) {
            result.m[i] = 0.0f;
            result.m[i + 1] = 0.0f;
        } else {
            const float alpha = result.m[i] / delta[i];
            const float beta = result.m[i + 1] / delta[i];
            const float tau = alpha * alpha + beta * beta;
            if (tau > 9.0f) {
                const float scale = 3.0f / std::sqrt(tau);
                result.m[i] = scale * alpha * delta[i];
                result.m[i + 1] = scale * beta * delta[i];
            }
        }
    }

    return result;
}

float evaluateHermiteSegment(float x, float x0, float x1, float y0, float y1, float m0, float m1)
{
    const float h = qMax(x1 - x0, kEpsilon);
    const float t = clampf((x - x0) / h, 0.0f, 1.0f);
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;
    return h00 * y0 + h10 * h * m0 + h01 * y1 + h11 * h * m1;
}

int findSegmentIndex(const QVector<QPointF> &points, float inputNits)
{
    if (points.isEmpty()) {
        return -1;
    }
    if (inputNits <= points.first().x()) {
        return 0;
    }
    if (inputNits >= points.last().x()) {
        return points.size() - 2;
    }
    for (int i = 0; i < points.size() - 1; ++i) {
        if (inputNits >= points[i].x() && inputNits <= points[i + 1].x()) {
            return i;
        }
    }
    return points.size() - 2;
}

QVector<QPointF> expandStepKnotsForEval(const QVector<QPointF> &points, const ToneCurveEndpoints &endpoints)
{
    QVector<QPointF> sorted = points;
    std::sort(sorted.begin(), sorted.end(), [](const QPointF &a, const QPointF &b) {
        return a.x() < b.x();
    });

    const float maxX = qMax(endpoints.sdrMaxPoint.x() - kEpsilon, kEpsilon);
    const float stepWidth = stepWidthForReference(qMax(endpoints.visualReferenceNits, 1.0f));

    QVector<QPointF> expanded;
    expanded.reserve(sorted.size() + 4);

    float groupX = 0.0f;
    float groupYMin = 0.0f;
    float groupYMax = 0.0f;
    bool inGroup = false;
    int groupCount = 0;

    auto flushGroup = [&]() {
        if (!inGroup) {
            return;
        }
        if (groupCount > 1 && groupYMax > groupYMin + kEpsilon) {
            const float stepX = clampf(groupX, kEpsilon, maxX);
            const float preX = qMax(kEpsilon, qMin(stepX - stepWidth, stepX - kEpsilon));
            expanded.append(QPointF(preX, groupYMin));
            expanded.append(QPointF(stepX, groupYMax));
        } else {
            expanded.append(QPointF(groupX, groupYMax));
        }
        inGroup = false;
        groupCount = 0;
    };

    for (const QPointF &point : sorted) {
        const float x = clampf(static_cast<float>(point.x()), kEpsilon, maxX);
        const float y = clampf(static_cast<float>(point.y()), 0.0f, endpoints.peakNits);

        if (!inGroup) {
            groupX = x;
            groupYMin = y;
            groupYMax = y;
            inGroup = true;
            groupCount = 1;
            continue;
        }

        if (qAbs(x - groupX) <= kSameXTolerance) {
            groupYMin = qMin(groupYMin, y);
            groupYMax = qMax(groupYMax, y);
            groupX = qMax(groupX, x);
            ++groupCount;
            continue;
        }

        flushGroup();
        groupX = x;
        groupYMin = y;
        groupYMax = y;
        inGroup = true;
        groupCount = 1;
    }

    flushGroup();
    return expanded;
}

} // namespace

QVector<QPointF> buildFullCurve(const ToneCurveEndpoints &endpoints, const QVector<QPointF> &intermediate)
{
    const QVector<QPointF> expanded = expandStepKnotsForEval(intermediate, endpoints);
    const QVector<QPointF> mids = sanitizeIntermediatePoints(expanded, endpoints);
    const QPointF sdrMax = sanitizeSdrMaxPoint(endpoints.sdrMaxPoint, endpoints, mids);

    QVector<QPointF> full;
    full.reserve(mids.size() + 2);
    full.append(QPointF(0.0f, 0.0f));
    for (const QPointF &point : mids) {
        full.append(point);
    }

    full.append(sdrMax);
    return full;
}

QVector<QPointF> sanitizeIntermediatePoints(const QVector<QPointF> &points, const ToneCurveEndpoints &endpoints)
{
    QVector<QPointF> sorted = points;
    std::sort(sorted.begin(), sorted.end(), [](const QPointF &a, const QPointF &b) {
        return a.x() < b.x();
    });

    const float maxX = qMax(endpoints.sdrMaxPoint.x() - kEpsilon, kEpsilon);
    QVector<QPointF> result;
    result.reserve(sorted.size());

    float lastX = 0.0f;
    float lastY = 0.0f;

    for (const QPointF &point : sorted) {
        float x = clampf(static_cast<float>(point.x()), kEpsilon, maxX);
        float y = clampf(static_cast<float>(point.y()), 0.0f, endpoints.peakNits);

        if (x <= lastX + kEpsilon) {
            x = lastX + kEpsilon;
        }
        if (y < lastY) {
            y = lastY;
        }
        if (x >= maxX) {
            continue;
        }

        result.append(QPointF(x, y));
        lastX = x;
        lastY = y;
    }

    return result;
}

QPointF sanitizeSdrMaxPoint(const QPointF &point, const ToneCurveEndpoints &endpoints,
                            const QVector<QPointF> &intermediate)
{
    float minX = kEpsilon;
    float minY = 0.0f;
    if (!intermediate.isEmpty()) {
        minX = qMax(minX, static_cast<float>(intermediate.last().x()) + kEpsilon);
        minY = qMax(minY, static_cast<float>(intermediate.last().y()));
    }

    const float maxInputX = qMax(endpoints.visualReferenceNits, kEpsilon);
    float x = clampf(static_cast<float>(point.x()), minX, maxInputX);
    float y = clampf(static_cast<float>(point.y()), minY, endpoints.peakNits);
    if (y < minY) {
        y = minY;
    }
    return QPointF(x, y);
}

float evaluateToneCurve(const QVector<QPointF> &fullCurve, float inputNits)
{
    if (fullCurve.isEmpty()) {
        return inputNits;
    }
    if (fullCurve.size() == 1) {
        return static_cast<float>(fullCurve.first().y());
    }

    if (inputNits <= fullCurve.first().x()) {
        return static_cast<float>(fullCurve.first().y());
    }
    if (inputNits >= fullCurve.last().x()) {
        return static_cast<float>(fullCurve.last().y());
    }

    if (fullCurve.size() == 2) {
        const float x0 = static_cast<float>(fullCurve[0].x());
        const float y0 = static_cast<float>(fullCurve[0].y());
        const float x1 = static_cast<float>(fullCurve[1].x());
        const float y1 = static_cast<float>(fullCurve[1].y());
        const float t = clampf((inputNits - x0) / qMax(x1 - x0, kEpsilon), 0.0f, 1.0f);
        return y0 + t * (y1 - y0);
    }

    const SegmentSlopes slopes = computeMonotonicSlopes(fullCurve);
    const int segment = findSegmentIndex(fullCurve, inputNits);
    const int i = qBound(0, segment, fullCurve.size() - 2);

    return evaluateHermiteSegment(inputNits, static_cast<float>(fullCurve[i].x()),
                                  static_cast<float>(fullCurve[i + 1].x()), static_cast<float>(fullCurve[i].y()),
                                  static_cast<float>(fullCurve[i + 1].y()), slopes.m[i], slopes.m[i + 1]);
}

void buildToneCurveLut(const QVector<QPointF> &fullCurve, float inputSpan, float referenceNits, float *lut, int size)
{
    if (!lut || size <= 0) {
        return;
    }

    const float span = qMax(inputSpan, kEpsilon);
    if (referenceNits <= 1.0f) {
        for (int i = 0; i < size; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(size - 1);
            const float inputNits = u * span;
            lut[i] = evaluateToneCurve(fullCurve, inputNits);
        }
        return;
    }

    const float ref = qMax(qMin(referenceNits, span), kEpsilon);
    const float logRef = std::log(ref);
    const float logSpan = std::log(span);

    for (int i = 0; i < size; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(size - 1);
        float inputNits;
        if (span <= ref * 1.001f) {
            inputNits = u * span;
        } else if (u <= 0.5f) {
            inputNits = (u / 0.5f) * ref;
        } else {
            const float t = (u - 0.5f) / 0.5f;
            inputNits = std::exp(logRef + t * (logSpan - logRef));
        }
        lut[i] = evaluateToneCurve(fullCurve, inputNits);
    }
}

QString formatToneCurvePoints(const QVector<QPointF> &points)
{
    QStringList parts;
    parts.reserve(points.size());
    for (const QPointF &point : points) {
        parts.append(QStringLiteral("%1:%2").arg(qRound(point.x())).arg(qRound(point.y())));
    }
    return parts.join(QLatin1Char(','));
}

QVector<QPointF> parseToneCurvePoints(const QString &encoded)
{
    QVector<QPointF> points;
    const QStringList tokens = encoded.split(QLatin1Char(','), Qt::SkipEmptyParts);
    points.reserve(tokens.size());
    for (const QString &token : tokens) {
        const QStringList pair = token.split(QLatin1Char(':'));
        if (pair.size() != 2) {
            continue;
        }
        bool okX = false;
        bool okY = false;
        const float x = pair[0].toFloat(&okX);
        const float y = pair[1].toFloat(&okY);
        if (okX && okY) {
            points.append(QPointF(x, y));
        }
    }
    return points;
}

QString formatSdrMaxPoint(const QPointF &point)
{
    return QStringLiteral("%1:%2").arg(qRound(point.x())).arg(qRound(point.y()));
}

QPointF parseSdrMaxPoint(const QString &encoded, const QPointF &fallback)
{
    const QStringList pair = encoded.split(QLatin1Char(':'));
    if (pair.size() != 2) {
        return fallback;
    }
    bool okX = false;
    bool okY = false;
    const float x = pair[0].toFloat(&okX);
    const float y = pair[1].toFloat(&okY);
    if (!okX || !okY) {
        return fallback;
    }
    return QPointF(x, y);
}

} // namespace AutoHdr
