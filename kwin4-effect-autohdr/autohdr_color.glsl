// AutoHDR color processing helpers (included into autohdr.frag)

#define MAX_AA_NEIGHBORS 12

const vec3 AUTOHDR_LUMA = vec3(0.2126, 0.7152, 0.0722);

// Curve-domain input shift only (tone curve x-axis black point). Do not use as pixel luma remap.
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

float luminanceYNits(vec3 rgbNits)
{
    return autohdrRec709ToXYZ(rgbNits).y;
}

// Soft toe stretch through ~0.25 relative luma (not a hard cut at 8/255).
float expandShadowDetailRel(float t)
{
    const float span = 0.25;
    if (t >= span) {
        return t;
    }
    float u = clamp(t / max(span, 1e-6), 0.0, 1.0);
    return pow(u, 0.75) * span;
}

// Content-aware shoulder separation: stretch the top band upward into headroom.
// Never pull highlights down — that left flat interiors darker than their edges.
float expandHighlightDetailRel(float t)
{
    const float knee = 1.0 - 8.0 / 255.0;
    const float kneeWidth = 8.0 / 255.0;
    const float ceiling = 1.05;
    if (t <= knee) {
        return t;
    }
    float u = clamp((t - knee) / max(1.0 - knee, 1e-6), 0.0, 1.0);
    float expanded = knee + pow(u, 0.75) * (ceiling - knee);
    return mix(t, expanded, smoothstep(knee, knee + kneeWidth * 0.5, t));
}

// Reposition within neighborhood luma envelope using wide-context hint (zero net lift).
vec3 applyRampInference(vec3 rgbNits, float refNits, float mask, float strength, float localMinNits,
                        float localMaxNits, float localAvgNits, float wideAvgNits, float localRangeNits)
{
    float span = localMaxNits - localMinNits;
    const float lsb = refNits / 255.0;
    if (span < lsb * 0.5) {
        return rgbNits;
    }

    float w = clamp(mask, 0.0, 1.0) * clamp(strength, 0.0, 1.0);
    if (w <= 1e-4) {
        return rgbNits;
    }

    float luma = max(luminanceYNits(rgbNits), 1e-6);
    float pos = clamp((luma - localMinNits) / span, 0.0, 1.0);
    float widePos = clamp((wideAvgNits - localMinNits) / span, 0.0, 1.0);
    float rangeRel = localRangeNits / max(refNits, 1.0);
    float rampGate = smoothstep(0.5 * lsb, 1.0 * lsb, rangeRel)
        * (1.0 - smoothstep(2.5 * lsb, 4.0 * lsb, rangeRel));

    float inferredPos = mix(pos, mix(pos, widePos, 0.4), w * rampGate);
    float newLuma = clamp(localMinNits + inferredPos * span, localMinNits, localMaxNits);
    return rgbNits * (newLuma / luma);
}

// Reconstruct smooth ramps under 8-bit quantization steps (mean-preserving).
vec3 applyDecontour(vec3 rgbNits, float refNits, float mask, float strength, float wideAvgNits,
                    float localRangeNits, float localAvgNits, float shadowWeight, float highlightWeight)
{
    float w = clamp(mask, 0.0, 1.0) * clamp(strength, 0.0, 1.0);
    if (w <= 1e-4 || wideAvgNits <= 0.0) {
        return rgbNits;
    }

    float luma = max(luminanceYNits(rgbNits), 1e-6);
    float avg = localAvgNits > 0.0 ? localAvgNits : luma;
    float rangeRel = localRangeNits / max(refNits, 1.0);
    const float lsb = 1.0 / 255.0;
    float rampBand = smoothstep(0.75 * lsb, 1.0 * lsb, rangeRel)
        * (1.0 - smoothstep(1.75 * lsb, 2.25 * lsb, rangeRel));
    float plateau = 1.0 - smoothstep(0.0, 0.75 * lsb, rangeRel);
    float decontourW = max(rampBand, plateau * 0.65) * w;

    float t = luma / max(refNits, 1.0);
    float crushed = 1.0 - smoothstep(0.05, 0.22, t);
    float shoulder = smoothstep(0.82, 0.96, t);
    float regionBlend = max(
        mix(0.40, 0.62, clamp(shadowWeight, 0.0, 1.0) * crushed),
        mix(0.35, 0.55, clamp(highlightWeight, 0.0, 1.0) * shoulder));

    float dev = luma - avg;
    float wideDev = wideAvgNits - avg;
    float hint = sign(wideDev != 0.0 ? wideDev : dev)
        * min(abs(wideDev), max(localRangeNits * 0.45, refNits * lsb));
    float targetDev = mix(dev, dev + hint * 0.65, decontourW * regionBlend);
    float newLuma = max(avg + targetDev, 1e-6);
    return rgbNits * (newLuma / luma);
}

// Mean-preserving shadow residual stretch (contrast only — no absolute toe lift).
vec3 applyShadowDetail(vec3 rgbNits, float refNits, float shadowMask, float strength, float localAvgNits,
                       float localRangeNits)
{
    float w = clamp(shadowMask, 0.0, 1.0) * clamp(strength, 0.0, 1.0);
    if (w <= 1e-4) {
        return rgbNits;
    }

    float luma = max(luminanceYNits(rgbNits), 1e-6);
    float avg = localAvgNits > 0.0 ? localAvgNits : luma;
    float t = luma / max(refNits, 1.0);
    float rangeRel = localRangeNits / max(refNits, 1.0);
    const float lsb = 1.0 / 255.0;
    float crushed = 1.0 - smoothstep(0.05, 0.22, t);
    float rampBand = smoothstep(0.75 * lsb, 1.0 * lsb, rangeRel)
        * (1.0 - smoothstep(1.75 * lsb, 2.25 * lsb, rangeRel));
    float flatGate = max(1.0 - smoothstep(1.0 * lsb, 2.5 * lsb, rangeRel), rampBand);

    float delta = luma - avg;
    float gain = mix(1.0, mix(1.85, 2.35, crushed), w * flatGate);
    float newLuma = max(avg + delta * gain, 1e-6);
    return rgbNits * (newLuma / luma);
}

// Mean-preserving highlight residual stretch (shoulder band, gated by quantization flatness).
vec3 applyHighlightDetail(vec3 rgbNits, float refNits, float highlightMask, float strength, float localAvgNits,
                          float localRangeNits)
{
    float w = clamp(highlightMask, 0.0, 1.0) * clamp(strength, 0.0, 1.0);
    if (w <= 1e-4) {
        return rgbNits;
    }

    float luma = max(luminanceYNits(rgbNits), 1e-6);
    float avg = localAvgNits > 0.0 ? localAvgNits : luma;
    float t = luma / max(refNits, 1.0);
    float shoulder = smoothstep(0.82, 0.96, t);
    const float lsb = 1.0 / 255.0;
    float rangeRel = localRangeNits / max(refNits, 1.0);
    float rampBand = smoothstep(0.75 * lsb, 1.0 * lsb, rangeRel)
        * (1.0 - smoothstep(1.75 * lsb, 2.25 * lsb, rangeRel));
    float flatGate = max(1.0 - smoothstep(1.0 * lsb, 2.5 * lsb, rangeRel), rampBand);

    float delta = luma - avg;
    float gain = mix(1.0, mix(1.55, 2.0, shoulder), w * shoulder * flatGate);
    float newLuma = max(avg + delta * gain, 1e-6);
    return rgbNits * (newLuma / luma);
}

// Multi-scale local contrast / perceptual depth (edge-aware).
vec3 applyDepthContrast(vec3 rgbNits, float localAvgNits, float wideAvgNits, float depthMask, float strength,
                        float refNits)
{
    float w = clamp(depthMask, 0.0, 1.0) * clamp(strength, 0.0, 1.0);
    if (w <= 1e-4 || localAvgNits <= 0.0) {
        return rgbNits;
    }

    float luma = max(luminanceYNits(rgbNits), 1e-6);
    float wide = wideAvgNits > 0.0 ? wideAvgNits : localAvgNits;
    float fineRel = abs(luma - localAvgNits) / max(refNits, 1.0);
    float edgeProtect = 1.0 - smoothstep(0.008, 0.04, fineRel);
    float gain = mix(1.0, 1.4, w * edgeProtect);
    float newLuma = max(wide + (luma - wide) * gain, 1e-6);
    return rgbNits * (newLuma / luma);
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

    const mat3 ap1D65ToXYZ = mat3(
        vec3(0.647507191, 0.266086400, -0.005448868),
        vec3(0.134379134, 0.675967813, 0.004072095),
        vec3(0.168569595, 0.057945795, 1.090434551)
    );
    float LumaAP1 = (ap1D65ToXYZ * ColorAP1).y;
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

float alphaCoherence(float centerA, float neighborA)
{
    float delta = abs(centerA - neighborA);
    return 1.0 - smoothstep(0.05, 0.25, delta);
}

float silhouetteEdgeWeight(float centerA, float neighborA)
{
    float delta = abs(centerA - neighborA);
    return smoothstep(0.15, 0.5, delta);
}

float spatialProcessingWeight(float centerA)
{
    return smoothstep(0.02, 0.15, centerA);
}

float microEdgeWeight(float lumaGradRel, int aaQuality)
{
    float rejectHigh = mix(0.18, 0.22, min(float(aaQuality), 2.0) * 0.5);
    return smoothstep(0.003, 0.006, lumaGradRel) * (1.0 - smoothstep(0.05, rejectHigh, lumaGradRel));
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
    float lumaNits = max(luminanceYNits(rgbNits), 1e-6);
    float t = applyUserBlackPoint(lumaNits / refNits, blackPointOffset);
    return t * refNits;
}

float poolCurveInputLuma(vec3 centerNits, vec3 neighborNits[MAX_AA_NEIGHBORS], float neighborDistWeights[MAX_AA_NEIGHBORS],
                       float centerAlpha, float neighborAlphas[MAX_AA_NEIGHBORS], int neighborCount,
                       float refNits, float blackPointOffset, float strength, float inputSpan, float maxSlope,
                       int aaQuality)
{
    strength *= spatialProcessingWeight(centerAlpha);
    if (strength <= 0.0 || neighborCount <= 0) {
        return 0.0;
    }

    float ref = max(refNits, 1.0);
    float centerLookup = curveLookupLumaNits(centerNits, ref, blackPointOffset);
    float centerRel = centerLookup / ref;
    float slopeW = curveSlopeWeight(centerLookup, inputSpan, maxSlope);
    float regionW = max(highlightRegionWeight(centerRel), slopeW);
    float midToneBoost = smoothstep(0.15, 0.45, centerRel) * (1.0 - smoothstep(0.85, 0.95, centerRel));
    float pooled = centerLookup;
    float weight = 1.0;

    for (int i = 0; i < neighborCount; ++i) {
        if (neighborAlphas[i] < 0.02) {
            continue;
        }

        float neighborLookup = curveLookupLumaNits(neighborNits[i], ref, blackPointOffset);
        float lumaGradRel = abs(neighborLookup - centerLookup) / ref;
        float microW = microEdgeWeight(lumaGradRel, aaQuality);
        float hueW = chromaCoherence(centerNits / ref, neighborNits[i] / ref);
        float alphaW = alphaCoherence(centerAlpha, neighborAlphas[i])
                     * (1.0 - silhouetteEdgeWeight(centerAlpha, neighborAlphas[i]));
        float localRegionW = max(regionW, midToneBoost * microW);
        float distW = neighborDistWeights[i];
        float w = microW * hueW * localRegionW * max(slopeW, 0.15) * alphaW * strength * distW;
        pooled += neighborLookup * w;
        weight += w;
    }

    return pooled / weight;
}

float poolCurveInputFromCross(vec3 setNits[5], float setAlphas[5], int centerIdx, float refNits,
                             float blackPointOffset, float strength, float inputSpan, float maxSlope, int aaQuality)
{
    vec3 neighbors[MAX_AA_NEIGHBORS];
    float alphas[MAX_AA_NEIGHBORS];
    float distWeights[MAX_AA_NEIGHBORS];
    int count = 0;
    for (int i = 0; i < 5; ++i) {
        if (i == centerIdx) {
            continue;
        }
        neighbors[count] = setNits[i];
        alphas[count] = setAlphas[i];
        distWeights[count] = 1.0;
        count++;
    }
    return poolCurveInputLuma(setNits[centerIdx], neighbors, distWeights, setAlphas[centerIdx], alphas, count,
                              refNits, blackPointOffset, strength, inputSpan, maxSlope, aaQuality);
}

float poolCurveInputFromSet(vec3 sampleNits, float sampleAlpha, vec3 allNits[MAX_AA_NEIGHBORS],
                            float allAlphas[MAX_AA_NEIGHBORS], float allDistWeights[MAX_AA_NEIGHBORS],
                            int sampleIdx, int totalCount, float refNits, float blackPointOffset, float strength,
                            float inputSpan, float maxSlope, int aaQuality)
{
    vec3 neighbors[MAX_AA_NEIGHBORS];
    float alphas[MAX_AA_NEIGHBORS];
    float distWeights[MAX_AA_NEIGHBORS];
    int count = 0;
    for (int i = 0; i < totalCount; ++i) {
        if (i == sampleIdx) {
            continue;
        }
        neighbors[count] = allNits[i];
        alphas[count] = allAlphas[i];
        distWeights[count] = allDistWeights[i];
        count++;
    }
    return poolCurveInputLuma(sampleNits, neighbors, distWeights, sampleAlpha, alphas, count, refNits,
                              blackPointOffset, strength, inputSpan, maxSlope, aaQuality);
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
    float outLuma = luminanceYNits(rgbNits) / ref;
    float shadowW = 1.0 - smoothstep(0.0, 0.08, outLuma);
    float edgeW = edgeFlatness(localGrad);
    float amp = strength * shadowW * edgeW;
    if (spatialAvgStrength > 0.0) {
        amp *= 1.0 - shadowW * 0.5;
    }
    if (amp <= 0.0) {
        return rgbNits;
    }
    vec3 rel = rgbNits / ref;
    rel += (ign(px) - 0.5) * amp;
    return rel * ref;
}

vec3 spatialAvgPostCurve(vec3 centerNits, vec3 neighborNits[MAX_AA_NEIGHBORS], float neighborDistWeights[MAX_AA_NEIGHBORS],
                         float centerAlpha, float neighborAlphas[MAX_AA_NEIGHBORS], int neighborCount, float strength,
                         float refNits, float localGrad, float curveAaStrength, int aaQuality)
{
    strength *= spatialProcessingWeight(centerAlpha);
    if (strength <= 0.0 || neighborCount <= 0) {
        return centerNits;
    }

    float ref = max(refNits, 1.0);
    float centerLuma = luminanceYNits(centerNits) / ref;
    if (centerLuma < 1e-6) {
        return centerNits;
    }

    float regionW = regionWeight(centerLuma);
    float edgeW = edgeFlatness(localGrad);
    float targetLuma = centerLuma;
    float weight = 1.0;
    vec3 chromaAccum = centerNits;
    float chromaWeight = 1.0;

    for (int i = 0; i < neighborCount; ++i) {
        if (neighborAlphas[i] < 0.02) {
            continue;
        }

        float neighborLuma = luminanceYNits(neighborNits[i]) / ref;
        float lumaGradRel = abs(neighborLuma - centerLuma);
        float microW = microEdgeWeight(lumaGradRel, aaQuality);
        float flatW = quantFlatness(neighborLuma - centerLuma) * regionW * edgeW;
        float silW = silhouetteEdgeWeight(centerAlpha, neighborAlphas[i]);
        float alphaW = alphaCoherence(centerAlpha, neighborAlphas[i]) * (1.0 - silW);
        float distW = neighborDistWeights[i];
        float w = max(flatW, microW * curveAaStrength) * alphaW * strength * distW;
        float blended = (centerLuma + neighborLuma) * 0.5;
        if (microW <= flatW || silW > 0.5) {
            blended = min(blended, centerLuma);
        }
        targetLuma += blended * w;
        weight += w;

        if (microW > flatW && curveAaStrength > 0.0) {
            float chromaMix = microW * curveAaStrength * 0.25 * w;
            vec3 chromaTarget = mix(centerNits, neighborNits[i], microW * curveAaStrength * 0.25);
            chromaAccum += chromaTarget * chromaMix;
            chromaWeight += chromaMix;
        }
    }

    if (curveAaStrength <= 0.0) {
        targetLuma = min(targetLuma / weight, centerLuma);
    } else {
        targetLuma = targetLuma / weight;
    }

    vec3 lumaResult = centerNits * (targetLuma / centerLuma);
    if (curveAaStrength > 0.0 && chromaWeight > 1.0) {
        vec3 chromaResult = chromaAccum / chromaWeight;
        float chromaLuma = luminanceYNits(chromaResult);
        chromaResult *= targetLuma * ref / max(chromaLuma, 1e-6);
        float chromaBlend = min((chromaWeight - 1.0) / max(weight, 1.0), 0.35);
        return mix(lumaResult, chromaResult, chromaBlend);
    }
    return lumaResult;
}
