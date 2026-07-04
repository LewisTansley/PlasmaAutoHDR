#version 140

#include "colormanagement.glsl"

#define TONE_CURVE_LUT_SIZE 1024

uniform sampler2D sampler;
uniform vec4 modulation;

uniform float blackPoint;
uniform float colorIntensity;
uniform float gamutExpansion;
uniform vec4 pqBoostParams;
uniform int perceptualColorEnabled;
uniform float toneCurveInputSpan;
uniform float toneCurveLut[TONE_CURVE_LUT_SIZE];
uniform float toneCurveSlopeLut[TONE_CURVE_LUT_SIZE];
uniform float toneCurveMaxSlope;
uniform float debandStrength;
uniform float ditherStrength;
uniform float curveAntialiasStrength;
uniform float highlightSoftness;
uniform int processingQuality;
uniform int antiAliasingQuality;
uniform int enableSpatialAvgPreCurve;
uniform int textureWidth;
uniform int textureHeight;
uniform sampler2D guidanceMap;
uniform sampler2D chromaMap;
uniform float aiStrength;
uniform float aiChromaStrength;
uniform int aiEnhanced;
uniform int aiChromaEnabled;

in vec2 texcoord0;
out vec4 fragColor;

#include "autohdr_color.glsl"
#include "autohdr_perceptual.glsl"
#include "autohdr_chroma.glsl"

float mapToneCurve(float inputNits, float inputSpan)
{
    float span = max(inputSpan, 1e-3);
    float u = clamp(inputNits / span, 0.0, 1.0);
    float idx = u * float(TONE_CURVE_LUT_SIZE - 1);
    int i0 = int(floor(idx));
    int i1 = min(i0 + 1, TONE_CURVE_LUT_SIZE - 1);
    float t = fract(idx);
    float y0 = toneCurveLut[i0];
    float y1 = toneCurveLut[i1];
    float m0 = toneCurveSlopeLut[i0];
    float m1 = toneCurveSlopeLut[i1];
    float t2 = t * t;
    float t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * y0 + (t3 - 2.0 * t2 + t) * m0
         + (-2.0 * t3 + 3.0 * t2) * y1 + (t3 - t2) * m1;
}

vec3 sampleSdrRel(sampler2D tex, vec2 uv, float ref, float sourceWhite)
{
    vec4 texSample = texture(tex, uv);
    float alpha = texSample.a;
    texSample = encodingToNits(texSample, sourceNamedTransferFunction, sourceTransferFunctionParams.x,
                             sourceTransferFunctionParams.y);
    texSample.rgb = (colorimetryTransform * vec4(texSample.rgb, 1.0)).rgb;
    texSample.rgb *= ref / sourceWhite;
    if (alpha < 0.02) {
        return texSample.rgb / ref;
    }
    return texSample.rgb / max(alpha, 0.001) / ref;
}

vec3 spatialAvgPreCurve(sampler2D tex, vec2 texcoord, vec3 centerRel, float strength, ivec2 texSize, float ref,
                        float sourceWhite, float centerAlpha)
{
    strength *= spatialProcessingWeight(centerAlpha);
    if (strength <= 0.0) {
        return centerRel;
    }

    float centerLuma = luminanceYNits(centerRel * ref) / ref;
    float regionW = regionWeight(centerLuma);

    ivec2 px = ivec2(clamp(texcoord * vec2(texSize), vec2(0.0), vec2(texSize - ivec2(1))));
    vec3 accum = centerRel;
    float count = 1.0;

    const ivec2 offsets[4] = ivec2[4](
        ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1)
    );

    for (int i = 0; i < 4; ++i) {
        ivec2 npx = clamp(px + offsets[i], ivec2(0), texSize - ivec2(1));
        vec2 nuv = (vec2(npx) + 0.5) / vec2(texSize);
        vec4 neighborSample = texture(tex, nuv);
        float neighborAlpha = neighborSample.a;
        if (neighborAlpha < 0.02) {
            continue;
        }

        vec3 neighborRel = sampleSdrRel(tex, nuv, ref, sourceWhite);
        float inputGrad = abs(luminanceYNits(neighborRel * ref) / ref - centerLuma);
        float flatW = 1.0 - smoothstep(0.001, 0.008, inputGrad);
        float rgbFlat = quantFlatness(neighborRel.r - centerRel.r)
                      * quantFlatness(neighborRel.g - centerRel.g)
                      * quantFlatness(neighborRel.b - centerRel.b);
        float alphaW = alphaCoherence(centerAlpha, neighborAlpha)
                     * (1.0 - silhouetteEdgeWeight(centerAlpha, neighborAlpha));
        float w = rgbFlat * regionW * flatW * alphaW * strength;
        accum += mix(centerRel, (centerRel + neighborRel) * 0.5, w);
        count += w;
    }

    return accum / count;
}

vec3 decodeRgbNits(vec4 tex, float ref, float sourceWhite)
{
    float rawAlpha = tex.a;
    tex = encodingToNits(tex, sourceNamedTransferFunction, sourceTransferFunctionParams.x,
                         sourceTransferFunctionParams.y);
    tex.rgb = (colorimetryTransform * vec4(tex.rgb, 1.0)).rgb;
    tex.rgb *= ref / sourceWhite;
    if (rawAlpha < 0.02) {
        return tex.rgb;
    }
    return tex.rgb / max(rawAlpha, 0.001);
}

vec4 sampleGuidance(vec2 uv)
{
    if (aiEnhanced <= 0 || aiStrength <= 1e-4) {
        return vec4(1.0, 0.0, 0.0, 0.0);
    }
    vec4 guide = texture(guidanceMap, uv);
    return vec4(max(guide.r, 1.0), clamp(guide.g, 0.0, 1.0), clamp(guide.b, 0.0, 1.0), clamp(guide.a, 0.0, 1.0));
}

vec4 sampleChroma(vec2 uv)
{
    if (aiChromaEnabled <= 0 || aiChromaStrength <= 1e-4) {
        return vec4(0.0, 0.0, 0.0, -1.0);
    }
    vec4 c = texture(chromaMap, uv);
    return vec4(
        clamp(c.r, 0.0, 1.0),
        c.g * 2.0 - 1.0,
        c.b * 2.0 - 1.0,
        c.a);
}

vec3 toneMapPipeline(vec3 rgb, float ref, float displayPeak, float curveSpan, float pooledCurveInputNits,
                     float effectiveHighlightSoftness, vec4 guidance, vec4 chromaGuide, float chromaStrength,
                     vec3 localChromaAp1, vec3 wideChromaAp1, vec3 neighborChromaAp1, float neighborChromaWeight,
                     float localAvgNits, float wideAvgNits, float localRangeNits, float localMinNits,
                     float localMaxNits)
{
    float strength = (aiEnhanced > 0) ? clamp(aiStrength, 0.0, 1.0) : 0.0;
    bool chromaOn = aiChromaEnabled > 0 && chromaStrength > 1e-4 && chromaGuide.r > 1e-4;
    float preCurveLuma = max(luminanceYNits(rgb), 1e-6);
    float tPre = preCurveLuma / max(ref, 1e-6);
    float shoulder = smoothstep(0.82, 0.96, tPre);

    if (strength > 1e-4) {
        float detailMask = max(max(guidance.b, guidance.a), guidance.g);

        rgb = applyRampInference(rgb, ref, guidance.b, strength, localMinNits, localMaxNits, localAvgNits,
                                 wideAvgNits, localRangeNits);
        rgb = applyDecontour(rgb, ref, detailMask, strength, wideAvgNits, localRangeNits, localAvgNits,
                             guidance.b, guidance.g);
        rgb = applyShadowDetail(rgb, ref, guidance.b, strength, localAvgNits, localRangeNits);

        rgb = reconstructHighlights(rgb, ref);

        rgb = applyRampInference(rgb, ref, guidance.g * shoulder, strength, localMinNits, localMaxNits,
                                 localAvgNits, wideAvgNits, localRangeNits);
        rgb = applyHighlightDetail(rgb, ref, guidance.g, strength, localAvgNits, localRangeNits);
        rgb = applyDepthContrast(rgb, localAvgNits, wideAvgNits, guidance.a, strength, ref);
    } else {
        rgb = reconstructHighlights(rgb, ref);
    }

    float rawLumaNits = max(luminanceYNits(rgb), 1e-6);
    float curveInputNits = applyUserBlackPoint(rawLumaNits / ref, blackPoint) * ref;
    float lookupNits = pooledCurveInputNits > 0.0 ? pooledCurveInputNits : curveInputNits;
    float outputNits = mapToneCurve(lookupNits, curveSpan);
    float scale = outputNits / max(rawLumaNits, 1e-6);

    if (perceptualColorEnabled > 0) {
        float localCi = -1.0;
        if (chromaOn && chromaGuide.a >= 0.0) {
            localCi = mix(colorIntensity, chromaGuide.a, chromaStrength * chromaGuide.r);
        }
        rgb = applyPerceptualLuminanceMap(rgb, rawLumaNits, outputNits, colorIntensity, pqBoostParams, localCi);

        if (chromaOn) {
            rgb = applyChromaRefinement(rgb, ref, chromaGuide, guidance, chromaStrength, wideChromaAp1,
                                        localChromaAp1, neighborChromaAp1, neighborChromaWeight);
        }

        if (gamutExpansion > 0.0) {
            rgb = expandGamutSmart(rgb / ref, gamutExpansion) * ref;
        }

        vec3 xyz = autohdrRec709ToXYZ(rgb);
        float outY = max(xyz.y, 1e-6);
        float limitedY = applyHighlightPeakLimit(outY, displayPeak, effectiveHighlightSoftness);
        rgb = autohdrXyzToRec709(xyz * (limitedY / outY));
    } else {
        rgb *= scale;

        if (chromaOn) {
            rgb = applyChromaRefinement(rgb, ref, chromaGuide, guidance, chromaStrength, wideChromaAp1,
                                        localChromaAp1, neighborChromaAp1, neighborChromaWeight);
        }

        if (gamutExpansion > 0.0) {
            rgb = expandGamutSmart(rgb / ref, gamutExpansion) * ref;
        }

        float outLuma = dot(rgb, AUTOHDR_LUMA);
        float limitedLuma = applyHighlightPeakLimit(outLuma, displayPeak, effectiveHighlightSoftness);
        rgb *= limitedLuma / max(outLuma, 1e-6);
    }

    // Post-curve highlight contrast: mean-preserving around scaled local avg (no global lift).
    if (strength > 1e-4 && localAvgNits > 0.0) {
        float luma = max(luminanceYNits(rgb), 1e-6);
        float conf = clamp(guidance.g, 0.0, 1.0);
        float scaledAvg = localAvgNits * (luma / max(preCurveLuma, 1e-6));
        float delta = luma - scaledAvg;
        float gain = mix(1.0, max(guidance.r, 1.0), conf * strength);
        float target = clamp(scaledAvg + delta * gain, 0.0, displayPeak);
        rgb *= target / luma;
    }

    return rgb;
}

vec3 sampleToneMappedNits(sampler2D tex, vec2 uv, float ref, float displayPeak, float sourceWhite, float curveSpan,
                          float pooledCurveInputNits, float effectiveHighlightSoftness)
{
    vec4 texSample = texture(tex, uv);
    return toneMapPipeline(decodeRgbNits(texSample, ref, sourceWhite), ref, displayPeak, curveSpan,
                           pooledCurveInputNits, effectiveHighlightSoftness, sampleGuidance(uv),
                           vec4(0.0, 0.0, 0.0, -1.0), 0.0, vec3(1.0), vec3(1.0), vec3(1.0), 0.0,
                           0.0, 0.0, 0.0, 0.0, 0.0);
}

void fetchNeighborSample(sampler2D tex, ivec2 px, ivec2 offset, ivec2 texSize, float ref, float sourceWhite,
                         out vec3 rgbNits, out float alpha)
{
    ivec2 npx = clamp(px + offset, ivec2(0), texSize - ivec2(1));
    vec4 neighborTex = texture(tex, (vec2(npx) + 0.5) / vec2(texSize));
    alpha = neighborTex.a;
    rgbNits = decodeRgbNits(neighborTex, ref, sourceWhite);
}

void main()
{
    vec4 tex = texture(sampler, texcoord0);
    float centerAlpha = tex.a;

    if (centerAlpha < 0.001) {
        fragColor = vec4(0.0);
        return;
    }

    float ref = max(destinationReferenceLuminance, 1.0);
    float displayPeak = max(maxDestinationLuminance, ref);
    float sourceWhite = max(sourceTransferFunctionParams.x + sourceTransferFunctionParams.y, 1.0);
    float curveSpan = toneCurveInputSpan > 1.0 ? toneCurveInputSpan : ref;
    bool fringeAlpha = centerAlpha < 0.02;
    float effectiveHighlightSoftness = highlightSoftness * spatialProcessingWeight(centerAlpha);

    vec3 rgb = decodeRgbNits(tex, ref, sourceWhite);

    float spatialAvg = processingQuality > 0 ? debandStrength : 0.0;
    float curveAa = processingQuality > 0 ? curveAntialiasStrength : 0.0;
    int aaQuality = clamp(antiAliasingQuality, 0, 2);
    ivec2 texSize = ivec2(0);
    if (textureWidth > 0 && textureHeight > 0) {
        texSize = ivec2(textureWidth, textureHeight);
    }

    bool enablePreCurve = enableSpatialAvgPreCurve > 0 || curveAa > 0.0;
    if (spatialAvg > 0.0 && enablePreCurve && texSize.x > 0) {
        vec3 centerRel = rgb / ref;
        centerRel = spatialAvgPreCurve(sampler, texcoord0, centerRel, spatialAvg, texSize, ref, sourceWhite,
                                       centerAlpha);
        rgb = centerRel * ref;
    }

    vec3 neighborInputs[MAX_AA_NEIGHBORS];
    float neighborAlphas[MAX_AA_NEIGHBORS];
    float neighborDistWeights[MAX_AA_NEIGHBORS];
    for (int i = 0; i < MAX_AA_NEIGHBORS; ++i) {
        neighborInputs[i] = rgb;
        neighborAlphas[i] = centerAlpha;
        neighborDistWeights[i] = 1.0;
    }

    int postCurveCount = 4;
    int poolCount = 4;
    float pooledCurveInput = 0.0;
    float pooledInputs[5];
    for (int i = 0; i < 5; ++i) {
        pooledInputs[i] = 0.0;
    }

    bool aiOn = aiEnhanced > 0 && aiStrength > 1e-4;
    bool chromaOn = aiChromaEnabled > 0 && aiChromaStrength > 1e-4;
    bool needNeighbors = curveAa > 0.0 || spatialAvg > 0.0 || ditherStrength > 0.0 || aiOn || chromaOn;
    if (texSize.x > 0 && needNeighbors) {
        ivec2 px = ivec2(clamp(texcoord0 * vec2(texSize), vec2(0.0), vec2(texSize - ivec2(1))));

        const ivec2 cardinalOffsets[4] = ivec2[4](
            ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1)
        );
        const ivec2 diagonalOffsets[4] = ivec2[4](
            ivec2(1, 1), ivec2(-1, 1), ivec2(1, -1), ivec2(-1, -1)
        );
        const ivec2 dist2Offsets[4] = ivec2[4](
            ivec2(2, 0), ivec2(-2, 0), ivec2(0, 2), ivec2(0, -2)
        );

        for (int i = 0; i < 4; ++i) {
            fetchNeighborSample(sampler, px, cardinalOffsets[i], texSize, ref, sourceWhite, neighborInputs[i],
                                neighborAlphas[i]);
            neighborDistWeights[i] = 1.0;
        }

        // AI detail reconstruction always needs r=1 and r=2 neighborhoods.
        if (aaQuality >= 1 || aiOn) {
            postCurveCount = 8;
            poolCount = 8;
            for (int i = 0; i < 4; ++i) {
                fetchNeighborSample(sampler, px, diagonalOffsets[i], texSize, ref, sourceWhite, neighborInputs[i + 4],
                                    neighborAlphas[i + 4]);
                neighborDistWeights[i + 4] = 0.707;
            }
        }

        if (aaQuality >= 2 || aiOn) {
            poolCount = 12;
            for (int i = 0; i < 4; ++i) {
                fetchNeighborSample(sampler, px, dist2Offsets[i], texSize, ref, sourceWhite, neighborInputs[i + 8],
                                    neighborAlphas[i + 8]);
                neighborDistWeights[i + 8] = 0.5;
            }
        }

        if (curveAa > 0.0) {
            vec3 crossNits[5];
            float crossAlphas[5];
            crossNits[0] = rgb;
            crossAlphas[0] = centerAlpha;
            for (int i = 0; i < 4; ++i) {
                crossNits[i + 1] = neighborInputs[i];
                crossAlphas[i + 1] = neighborAlphas[i];
            }

            if (aaQuality >= 2 || aiOn) {
                pooledCurveInput = poolCurveInputLuma(rgb, neighborInputs, neighborDistWeights, centerAlpha,
                                                      neighborAlphas, poolCount, ref, blackPoint, curveAa, curveSpan,
                                                      toneCurveMaxSlope, aaQuality);
            } else {
                pooledCurveInput = poolCurveInputFromCross(crossNits, crossAlphas, 0, ref, blackPoint, curveAa,
                                                           curveSpan, toneCurveMaxSlope, aaQuality);
            }
            for (int i = 0; i < 5; ++i) {
                pooledInputs[i] = poolCurveInputFromCross(crossNits, crossAlphas, i, ref, blackPoint, curveAa,
                                                          curveSpan, toneCurveMaxSlope, aaQuality);
            }
        }
    }

    vec4 centerGuidance = sampleGuidance(texcoord0);
    vec4 centerChroma = sampleChroma(texcoord0);
    float chromaStrength = clamp(aiChromaStrength, 0.0, 1.0);
    vec3 localChromaAp1 = vec3(1.0);
    vec3 wideChromaAp1 = vec3(1.0);
    vec3 neighborChromaAp1 = vec3(1.0);
    float neighborChromaWeight = 0.0;
    float localAvgNits = 0.0;
    float wideAvgNits = 0.0;
    float localRangeNits = 0.0;
    float localMinNits = 0.0;
    float localMaxNits = 0.0;
    if ((aiOn || chromaOn) && texSize.x > 0) {
        float centerL = luminanceYNits(rgb);
        float localAccum = centerL;
        float localMin = centerL;
        float localMax = centerL;
        vec3 chromaAccum = autohdrRgbRelToAp1Chroma(rgb / max(ref, 1.0));
        for (int i = 0; i < 8; ++i) {
            float nL = luminanceYNits(neighborInputs[i]);
            localAccum += nL;
            localMin = min(localMin, nL);
            localMax = max(localMax, nL);
            chromaAccum += autohdrRgbRelToAp1Chroma(neighborInputs[i] / max(ref, 1.0));
        }
        localAvgNits = localAccum / 9.0;
        localMinNits = localMin;
        localMaxNits = localMax;
        localRangeNits = localMax - localMin;
        localChromaAp1 = chromaAccum / 9.0;

        float wideAccum = localAccum;
        vec3 wideChromaAccum = chromaAccum;
        for (int i = 8; i < 12; ++i) {
            wideAccum += luminanceYNits(neighborInputs[i]);
            wideChromaAccum += autohdrRgbRelToAp1Chroma(neighborInputs[i] / max(ref, 1.0));
        }
        wideAvgNits = wideAccum / 13.0;
        wideChromaAp1 = wideChromaAccum / 13.0;
        neighborChromaAp1 = wideChromaAp1;
        neighborChromaWeight = 1.0;
    }
    rgb = toneMapPipeline(rgb, ref, displayPeak, curveSpan, pooledCurveInput, effectiveHighlightSoftness,
                          centerGuidance, centerChroma, chromaStrength, localChromaAp1, wideChromaAp1,
                          neighborChromaAp1, neighborChromaWeight, localAvgNits, wideAvgNits, localRangeNits,
                          localMinNits, localMaxNits);

    float localGrad = 0.0;
    vec3 toneMappedNeighbors[MAX_AA_NEIGHBORS];
    for (int i = 0; i < MAX_AA_NEIGHBORS; ++i) {
        toneMappedNeighbors[i] = rgb;
    }

    if ((spatialAvg > 0.0 || ditherStrength > 0.0) && texSize.x > 0) {
        float centerLuma = luminanceYNits(rgb) / ref;
        float gradAccum = 0.0;
        ivec2 px = ivec2(clamp(texcoord0 * vec2(texSize), vec2(0.0), vec2(texSize - ivec2(1))));
        const ivec2 neighborOffsets[12] = ivec2[12](
            ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1),
            ivec2(1, 1), ivec2(-1, 1), ivec2(1, -1), ivec2(-1, -1),
            ivec2(2, 0), ivec2(-2, 0), ivec2(0, 2), ivec2(0, -2)
        );

        for (int i = 0; i < postCurveCount; ++i) {
            float neighborPooled = 0.0;
            if (curveAa > 0.0) {
                if (i < 4) {
                    neighborPooled = pooledInputs[i + 1];
                } else {
                    neighborPooled = poolCurveInputFromSet(neighborInputs[i], neighborAlphas[i], neighborInputs,
                                                           neighborAlphas, neighborDistWeights, i, postCurveCount,
                                                           ref, blackPoint, curveAa, curveSpan, toneCurveMaxSlope,
                                                           aaQuality);
                }
            }
            ivec2 npx = clamp(px + neighborOffsets[i], ivec2(0), texSize - ivec2(1));
            vec2 nuv = (vec2(npx) + 0.5) / vec2(texSize);
            toneMappedNeighbors[i] =
                toneMapPipeline(neighborInputs[i], ref, displayPeak, curveSpan, neighborPooled,
                                highlightSoftness * spatialProcessingWeight(neighborAlphas[i]),
                                sampleGuidance(nuv), vec4(0.0, 0.0, 0.0, -1.0), 0.0, vec3(1.0), vec3(1.0),
                                vec3(1.0), 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
            gradAccum += abs(luminanceYNits(toneMappedNeighbors[i]) / ref - centerLuma);
        }

        localGrad = gradAccum / float(postCurveCount);
    }

    if (spatialAvg > 0.0) {
        // Do not immediately average away shadow/midtone detail reconstruction.
        float detailProtect = 0.0;
        if (aiOn) {
            detailProtect = (centerGuidance.b * 0.25 + centerGuidance.a * 0.35) * clamp(aiStrength, 0.0, 1.0);
        }
        float debandW = spatialAvg * (1.0 - detailProtect);
        if (debandW > 1e-4) {
            rgb = spatialAvgPostCurve(rgb, toneMappedNeighbors, neighborDistWeights, centerAlpha, neighborAlphas,
                                      postCurveCount, debandW, ref, localGrad, curveAa, aaQuality);
        }
    }

    rgb = luminanceScaledDither(rgb, gl_FragCoord.xy, ditherStrength, ref, localGrad, spatialAvg);

    if (fringeAlpha) {
        tex.rgb = max(rgb, vec3(0.0));
    } else {
        tex.rgb = max(rgb * centerAlpha, vec3(0.0));
    }
    tex.a = centerAlpha;
    tex *= modulation;
    fragColor = nitsToDestinationEncoding(tex);
}
