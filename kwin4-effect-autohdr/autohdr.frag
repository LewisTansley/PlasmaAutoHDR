#version 140

#include "colormanagement.glsl"

#define TONE_CURVE_LUT_SIZE 1024

uniform sampler2D sampler;
uniform vec4 modulation;

uniform float blackPoint;
uniform float colorVibrance;
uniform float gamutExpansion;
uniform float toneCurveInputSpan;
uniform float toneCurveLut[TONE_CURVE_LUT_SIZE];
uniform float toneCurveSlopeLut[TONE_CURVE_LUT_SIZE];
uniform float toneCurveMaxSlope;
uniform float debandStrength;
uniform float ditherStrength;
uniform float curveAntialiasStrength;
uniform float highlightSoftness;
uniform int processingQuality;
uniform int enableSpatialAvgPreCurve;
uniform int textureWidth;
uniform int textureHeight;

in vec2 texcoord0;
out vec4 fragColor;

#include "autohdr_color.glsl"

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

vec3 applyColorControls(vec3 rgb, float sat, float vib)
{
    float luma = dot(rgb, AUTOHDR_LUMA);
    float maxVal = max(rgb.r, max(rgb.g, rgb.b));
    float minVal = min(rgb.r, min(rgb.g, rgb.b));
    float lnm = maxVal - minVal;

    if (abs(vib) > 0.001) {
        float amt = vib * (1.0 - lnm);
        rgb = mix(vec3(luma), rgb, 1.0 + amt);
    }
    return mix(vec3(luma), rgb, sat);
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

    float centerLuma = dot(centerRel, AUTOHDR_LUMA);
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
        float inputGrad = abs(dot(neighborRel, AUTOHDR_LUMA) - centerLuma);
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

vec3 toneMapPipeline(vec3 rgb, float ref, float displayPeak, float curveSpan, float pooledCurveInputNits,
                     float effectiveHighlightSoftness)
{
    rgb = reconstructHighlights(rgb, ref);

    float lumaNits = max(dot(rgb, AUTOHDR_LUMA), 1e-6);
    float t = lumaNits / ref;
    t = applyUserBlackPoint(t, blackPoint);

    float inputNits = t * ref;
    float lookupNits = pooledCurveInputNits > 0.0 ? pooledCurveInputNits : inputNits;
    float outputNits = mapToneCurve(lookupNits, curveSpan);
    rgb *= outputNits / max(inputNits, 1e-6);

    if (gamutExpansion > 0.0) {
        rgb = expandGamutSmart(rgb / ref, gamutExpansion) * ref;
    }

    vec3 sceneMapped = applyColorControls(rgb / ref, 1.0, colorVibrance);
    rgb = sceneMapped * ref;

    float outLuma = dot(rgb, AUTOHDR_LUMA);
    float limitedLuma = applyHighlightPeakLimit(outLuma, displayPeak, effectiveHighlightSoftness);
    rgb *= limitedLuma / max(outLuma, 1e-6);

    return rgb;
}

vec3 sampleToneMappedNits(sampler2D tex, vec2 uv, float ref, float displayPeak, float sourceWhite, float curveSpan,
                          float effectiveHighlightSoftness)
{
    vec4 texSample = texture(tex, uv);
    return toneMapPipeline(decodeRgbNits(texSample, ref, sourceWhite), ref, displayPeak, curveSpan, 0.0,
                           effectiveHighlightSoftness);
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
    ivec2 texSize = ivec2(0);
    if (textureWidth > 0 && textureHeight > 0) {
        texSize = ivec2(textureWidth, textureHeight);
    }

    if (spatialAvg > 0.0 && enableSpatialAvgPreCurve > 0 && texSize.x > 0) {
        vec3 centerRel = rgb / ref;
        centerRel = spatialAvgPreCurve(sampler, texcoord0, centerRel, spatialAvg, texSize, ref, sourceWhite,
                                       centerAlpha);
        rgb = centerRel * ref;
    }

    vec3 neighborInput0 = rgb;
    vec3 neighborInput1 = rgb;
    vec3 neighborInput2 = rgb;
    vec3 neighborInput3 = rgb;
    float neighborAlpha0 = centerAlpha;
    float neighborAlpha1 = centerAlpha;
    float neighborAlpha2 = centerAlpha;
    float neighborAlpha3 = centerAlpha;
    float pooledCurveInput = 0.0;

    if (texSize.x > 0 && (curveAa > 0.0 || spatialAvg > 0.0 || ditherStrength > 0.0)) {
        ivec2 px = ivec2(clamp(texcoord0 * vec2(texSize), vec2(0.0), vec2(texSize - ivec2(1))));

        ivec2 npx0 = clamp(px + ivec2(1, 0), ivec2(0), texSize - ivec2(1));
        vec4 neighborTex0 = texture(sampler, (vec2(npx0) + 0.5) / vec2(texSize));
        neighborAlpha0 = neighborTex0.a;
        neighborInput0 = decodeRgbNits(neighborTex0, ref, sourceWhite);

        ivec2 npx1 = clamp(px + ivec2(-1, 0), ivec2(0), texSize - ivec2(1));
        vec4 neighborTex1 = texture(sampler, (vec2(npx1) + 0.5) / vec2(texSize));
        neighborAlpha1 = neighborTex1.a;
        neighborInput1 = decodeRgbNits(neighborTex1, ref, sourceWhite);

        ivec2 npx2 = clamp(px + ivec2(0, 1), ivec2(0), texSize - ivec2(1));
        vec4 neighborTex2 = texture(sampler, (vec2(npx2) + 0.5) / vec2(texSize));
        neighborAlpha2 = neighborTex2.a;
        neighborInput2 = decodeRgbNits(neighborTex2, ref, sourceWhite);

        ivec2 npx3 = clamp(px + ivec2(0, -1), ivec2(0), texSize - ivec2(1));
        vec4 neighborTex3 = texture(sampler, (vec2(npx3) + 0.5) / vec2(texSize));
        neighborAlpha3 = neighborTex3.a;
        neighborInput3 = decodeRgbNits(neighborTex3, ref, sourceWhite);

        if (curveAa > 0.0) {
            vec3 neighborInputs[4] = vec3[4](neighborInput0, neighborInput1, neighborInput2, neighborInput3);
            float neighborAlphas[4] = float[4](neighborAlpha0, neighborAlpha1, neighborAlpha2, neighborAlpha3);
            pooledCurveInput = poolCurveInputLuma(rgb, neighborInputs, centerAlpha, neighborAlphas, ref, blackPoint,
                                                  curveAa, curveSpan, toneCurveMaxSlope);
        }
    }

    rgb = toneMapPipeline(rgb, ref, displayPeak, curveSpan, pooledCurveInput, effectiveHighlightSoftness);

    float localGrad = 0.0;
    vec3 neighbor0 = rgb;
    vec3 neighbor1 = rgb;
    vec3 neighbor2 = rgb;
    vec3 neighbor3 = rgb;
    if ((spatialAvg > 0.0 || ditherStrength > 0.0) && texSize.x > 0) {
        float centerLuma = dot(rgb / ref, AUTOHDR_LUMA);
        float gradAccum = 0.0;

        neighbor0 = toneMapPipeline(neighborInput0, ref, displayPeak, curveSpan, 0.0,
                                    highlightSoftness * spatialProcessingWeight(neighborAlpha0));
        gradAccum += abs(dot(neighbor0 / ref, AUTOHDR_LUMA) - centerLuma);

        neighbor1 = toneMapPipeline(neighborInput1, ref, displayPeak, curveSpan, 0.0,
                                    highlightSoftness * spatialProcessingWeight(neighborAlpha1));
        gradAccum += abs(dot(neighbor1 / ref, AUTOHDR_LUMA) - centerLuma);

        neighbor2 = toneMapPipeline(neighborInput2, ref, displayPeak, curveSpan, 0.0,
                                    highlightSoftness * spatialProcessingWeight(neighborAlpha2));
        gradAccum += abs(dot(neighbor2 / ref, AUTOHDR_LUMA) - centerLuma);

        neighbor3 = toneMapPipeline(neighborInput3, ref, displayPeak, curveSpan, 0.0,
                                    highlightSoftness * spatialProcessingWeight(neighborAlpha3));
        gradAccum += abs(dot(neighbor3 / ref, AUTOHDR_LUMA) - centerLuma);

        localGrad = gradAccum * 0.25;
    }

    if (spatialAvg > 0.0) {
        float neighborAlphas[4] = float[4](neighborAlpha0, neighborAlpha1, neighborAlpha2, neighborAlpha3);
        rgb = spatialAvgPostCurve(rgb, neighbor0, neighbor1, neighbor2, neighbor3, centerAlpha, neighborAlphas,
                                  spatialAvg, ref, localGrad, curveAa);
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
