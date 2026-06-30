// AutoHDR color processing helpers (included into autohdr.frag)

const vec3 AUTOHDR_LUMA = vec3(0.2126, 0.7152, 0.0722);

float applyUserBlackPoint(float t, float offset)
{
    return max(t - offset, 0.0) / max(1.0 - offset, 1e-6);
}

vec3 reconstructHighlights(vec3 rgbNits, float refNits)
{
    vec3 rel = rgbNits / max(refNits, 1.0);
    float peak = max(max(rel.r, rel.g), rel.b);
    if (peak <= 0.98) {
        return rgbNits;
    }

    float minChannel = min(rel.r, min(rel.g, rel.b));
    float nearClip = step(0.98, peak);
    float channelSpread = (peak - minChannel) / max(peak, 1e-4);
    float clipMask = nearClip * step(0.05, channelSpread);
    if (clipMask <= 0.0) {
        return rgbNits;
    }

    float luma = dot(rel, AUTOHDR_LUMA);
    vec3 unclipped = rel / max(peak, 1e-4);
    float unclippedLuma = dot(unclipped, AUTOHDR_LUMA);
    vec3 reconstructed = unclipped * (luma / max(unclippedLuma, 1e-4));
    return mix(rel, reconstructed, clipMask) * refNits;
}

vec3 autohdrRec709ToXYZ(vec3 linearRec709)
{
    return vec3(
        dot(linearRec709, vec3(0.412390798, 0.357584327, 0.180480793)),
        dot(linearRec709, vec3(0.212639004, 0.715168655, 0.072192319)),
        dot(linearRec709, vec3(0.019330818, 0.119194783, 0.950532138))
    );
}

vec3 autohdrAp1D65ToRec709(vec3 linearAP1)
{
    const mat3 ap1D65ToXYZ = mat3(
        vec3(0.647507191, 0.266086400, -0.005448868),
        vec3(0.134379134, 0.675967813, 0.004072095),
        vec3(0.168569595, 0.057945795, 1.090434551)
    );
    const mat3 xyzToRec709 = mat3(
        vec3(3.240969896, -0.969243646, 0.055630080),
        vec3(-1.537383198, 1.875967503, -0.203976959),
        vec3(-0.498610765, 0.041555058, 1.056971550)
    );
    return xyzToRec709 * (ap1D65ToXYZ * linearAP1);
}

vec3 expandGamutSmart(vec3 vHDRColor, float userBoost)
{
    if (userBoost <= 0.0) {
        return vHDRColor;
    }

    float fExpandGamut = userBoost;

    const mat3 sRGB_2_AP1_D65 = mat3(
        vec3(0.616850994, 0.069866394, 0.020549067),
        vec3(0.334062934, 0.917416679, 0.107642211),
        vec3(0.049086072, 0.012716927, 0.871808722)
    );
    const mat3 AP1_D65_2_sRGB = mat3(
        vec3(1.692679398, -0.128573980, -0.024022465),
        vec3(-0.606218057, 1.137933633, -0.126211718),
        vec3(-0.086461341, -0.009359653, 1.150234183)
    );
    const mat3 Wide_2_AP1_D65 = mat3(
        vec3(0.834516905, 0.025545194, 0.001925829),
        vec3(0.160259590, 0.973101532, 0.030372797),
        vec3(0.005223505, 0.001353275, 0.967701374)
    );
    const mat3 AP1_2_sRGB = mat3(
        vec3(1.70505, -0.13026, -0.02400),
        vec3(-0.62179, 1.14080, -0.12897),
        vec3(-0.08326, -0.01055, 1.15297)
    );

    const mat3 ExpandMat = Wide_2_AP1_D65 * AP1_D65_2_sRGB;
    vec3 ColorAP1 = sRGB_2_AP1_D65 * vHDRColor;

    float LumaAP1 = autohdrRec709ToXYZ(autohdrAp1D65ToRec709(ColorAP1)).y;
    vec3 ChromaAP1 = ColorAP1 / max(LumaAP1, 1e-6);

    float ChromaDistSqr = dot(ChromaAP1 - 1.0, ChromaAP1 - 1.0);
    ChromaDistSqr = max(abs(ChromaDistSqr), 0.000001);

    float ExpandAmount = (1.0 - exp2(-4.0 * ChromaDistSqr))
                       * (1.0 - exp2(-4.0 * fExpandGamut * LumaAP1 * LumaAP1));

    vec3 ColorExpand = ExpandMat * ColorAP1;
    ColorAP1 = mix(ColorAP1, ColorExpand, ExpandAmount);

    return AP1_2_sRGB * ColorAP1;
}

float quantFlatness(float delta)
{
    const float s = 1.0 / 255.0;
    float d = abs(delta);
    return 1.0 - smoothstep(s + 1.0e-4, s * 2.0, d);
}

float regionWeight(float lumaRel)
{
    float shadowW = 1.0 - smoothstep(0.0, 0.12, lumaRel);
    float highlightW = smoothstep(0.85, 0.98, lumaRel);
    return max(shadowW, highlightW);
}

float edgeFlatness(float localGrad)
{
    return 1.0 - smoothstep(0.002, 0.015, localGrad);
}

float microEdgeWeight(float lumaGradRel)
{
    return smoothstep(0.006, 0.012, lumaGradRel) * (1.0 - smoothstep(0.05, 0.12, lumaGradRel));
}

float chromaCoherence(vec3 centerRel, vec3 neighborRel)
{
    vec3 delta = abs(neighborRel - centerRel);
    float maxDelta = max(max(delta.r, delta.g), delta.b);
    return 1.0 - smoothstep(0.02, 0.08, maxDelta);
}

float highlightRegionWeight(float lumaRel)
{
    return smoothstep(0.55, 0.92, lumaRel);
}

float curveSlopeWeight(float lookupNits, float inputSpan, float maxSlope)
{
    if (maxSlope <= 1e-6) {
        return 0.0;
    }

    float span = max(inputSpan, 1e-3);
    float u = clamp(lookupNits / span, 0.0, 1.0);
    int idx = int(round(u * float(TONE_CURVE_LUT_SIZE - 1)));
    idx = clamp(idx, 0, TONE_CURVE_LUT_SIZE - 1);
    float normalizedSlope = abs(toneCurveSlopeLut[idx]) / maxSlope;
    return smoothstep(0.25, 0.85, normalizedSlope);
}

float curveLookupLumaNits(vec3 rgbNits, float refNits, float blackPointOffset)
{
    float lumaNits = max(dot(rgbNits, AUTOHDR_LUMA), 1e-6);
    float t = applyUserBlackPoint(lumaNits / refNits, blackPointOffset);
    return t * refNits;
}

float poolCurveInputLuma(vec3 centerNits, vec3 neighborNits[4], float refNits, float blackPointOffset,
                       float strength, float inputSpan, float maxSlope)
{
    if (strength <= 0.0) {
        return 0.0;
    }

    float ref = max(refNits, 1.0);
    float centerLookup = curveLookupLumaNits(centerNits, ref, blackPointOffset);
    float centerRel = centerLookup / ref;
    float pooled = centerLookup;
    float weight = 1.0;

    for (int i = 0; i < 4; ++i) {
        float neighborLookup = curveLookupLumaNits(neighborNits[i], ref, blackPointOffset);
        float lumaGradRel = abs(neighborLookup - centerLookup) / ref;
        float microW = microEdgeWeight(lumaGradRel);
        float hueW = chromaCoherence(centerNits / ref, neighborNits[i] / ref);
        float highlightW = highlightRegionWeight(centerRel);
        float slopeW = curveSlopeWeight(centerLookup, inputSpan, maxSlope);
        float w = microW * hueW * highlightW * max(slopeW, 0.15) * strength;
        pooled += neighborLookup * w;
        weight += w;
    }

    return pooled / weight;
}

float softHighlightShoulder(float luma, float displayPeak, float softness)
{
    if (softness <= 0.0 || luma <= displayPeak) {
        return min(luma, displayPeak);
    }

    float kneeStart = mix(displayPeak, displayPeak * 0.85, softness);
    if (luma <= kneeStart) {
        return luma;
    }

    float range = max(displayPeak - kneeStart, 1e-6);
    float t = (luma - kneeStart) / range;
    return kneeStart + range * (1.0 - exp(-t));
}

float applyHighlightPeakLimit(float outLuma, float displayPeak, float softness)
{
    if (softness > 0.0) {
        return softHighlightShoulder(outLuma, displayPeak, softness);
    }
    return min(outLuma, displayPeak);
}

float ign(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec3 luminanceScaledDither(vec3 rgbNits, vec2 px, float strength, float refNits, float localGrad,
                           float spatialAvgStrength)
{
    if (strength <= 0.0) {
        return rgbNits;
    }

    float ref = max(refNits, 1.0);
    vec3 rel = rgbNits / ref;
    float outLuma = dot(rel, AUTOHDR_LUMA);
    float shadowW = 1.0 - smoothstep(0.0, 0.08, outLuma);
    float edgeW = edgeFlatness(localGrad);
    float amp = strength * shadowW * edgeW;
    if (spatialAvgStrength > 0.0) {
        amp *= 1.0 - shadowW * 0.5;
    }
    if (amp <= 0.0) {
        return rgbNits;
    }
    rel += (ign(px) - 0.5) * amp;
    return rel * ref;
}

vec3 spatialAvgPostCurve(vec3 centerNits, vec3 neighbor0, vec3 neighbor1, vec3 neighbor2, vec3 neighbor3,
                         float strength, float refNits, float localGrad, float curveAaStrength)
{
    if (strength <= 0.0) {
        return centerNits;
    }

    float ref = max(refNits, 1.0);
    float centerLuma = dot(centerNits / ref, AUTOHDR_LUMA);
    if (centerLuma < 1e-6) {
        return centerNits;
    }

    float regionW = regionWeight(centerLuma);
    float edgeW = edgeFlatness(localGrad);
    float targetLuma = centerLuma;
    float weight = 1.0;

    vec3 neighbors[4] = vec3[4](neighbor0, neighbor1, neighbor2, neighbor3);
    for (int i = 0; i < 4; ++i) {
        float neighborLuma = dot(neighbors[i] / ref, AUTOHDR_LUMA);
        float lumaGradRel = abs(neighborLuma - centerLuma);
        float microW = microEdgeWeight(lumaGradRel);
        float flatW = quantFlatness(neighborLuma - centerLuma) * regionW * edgeW;
        float w = max(flatW, microW * curveAaStrength) * strength;
        float blended = (centerLuma + neighborLuma) * 0.5;
        if (microW <= flatW) {
            blended = min(blended, centerLuma);
        }
        targetLuma += blended * w;
        weight += w;
    }

    if (curveAaStrength <= 0.0) {
        targetLuma = min(targetLuma / weight, centerLuma);
    } else {
        targetLuma = targetLuma / weight;
    }
    return centerNits * (targetLuma / centerLuma);
}
