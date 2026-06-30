#version 140

#include "colormanagement.glsl"

uniform sampler2D sampler;
uniform vec4 modulation;

uniform float blackPoint;
uniform float colorVibrance;
uniform float gamutExpansion;
uniform float chromaCompensation;
uniform float highlightRolloff;
uniform float gamutMappingStrength;
uniform float postCurveDebandStrength;
uniform int captureUsesFloat;
uniform float spatialHighlightRecovery;
uniform float toneCurveInputSpan;
uniform float toneCurveReferenceNits;
uniform float minDisplayNits;
uniform float toneCurveLut[512];
uniform float ditherStrength;
uniform int processingQuality;
uniform int textureWidth;
uniform int textureHeight;

in vec2 texcoord0;
out vec4 fragColor;

#include "autohdr_color.glsl"

float toneCurveU(float inputNits, float inputSpan, float referenceNits)
{
    float span = max(inputSpan, 1e-3);
    inputNits = clamp(inputNits, 0.0, span);

    if (referenceNits <= 1.0) {
        return inputNits / span;
    }

    float ref = max(min(referenceNits, span), 1e-3);
    if (span <= ref * 1.001) {
        return inputNits / span;
    }

    float uLinear = (inputNits / ref) * 0.5;
    if (inputNits <= ref) {
        return uLinear;
    }

    float logRef = log(ref);
    float logSpan = log(span);
    float t = (log(inputNits) - logRef) / max(logSpan - logRef, 1e-4);
    float uLog = 0.5 + t * 0.5;
    float blend = smoothstep(ref * 0.95, ref * 1.05, inputNits);
    return mix(uLinear, uLog, blend);
}

float mapToneCurve(float inputNits, float inputSpan, float referenceNits)
{
    float u = toneCurveU(inputNits, inputSpan, referenceNits);
    float idx = u * 511.0;
    int i0 = int(floor(idx));
    int i1 = min(i0 + 1, 511);
    float f = fract(idx);
    return mix(toneCurveLut[i0], toneCurveLut[i1], f);
}

float applyToneCurveMapping(float t, float ref, float curveInputSpan, out float correctedNits)
{
    if (toneCurveReferenceNits > 1.0) {
        float curveRef = toneCurveReferenceNits;
        correctedNits = t * curveRef;
        float span = curveInputSpan > 1.0 ? curveInputSpan : curveRef;
        return mapToneCurve(correctedNits, span, curveRef);
    }

    correctedNits = t * ref;
    float span = curveInputSpan > 1.0 ? curveInputSpan : ref;
    return mapToneCurve(correctedNits, span, 0.0);
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

vec3 decodeToNits(vec4 tex, float ref, float sourceWhite)
{
    tex = encodingToNits(tex, sourceNamedTransferFunction, sourceTransferFunctionParams.x,
                         sourceTransferFunctionParams.y);
    tex.rgb = (colorimetryTransform * vec4(tex.rgb, 1.0)).rgb;
    tex.rgb *= ref / sourceWhite;
    float alpha = max(tex.a, 0.001);
    return tex.rgb / alpha;
}

vec3 processPixelToOutput(sampler2D tex, vec2 uv, ivec2 texSize, float ref, float displayPeak, float sourceWhite,
                          float curveSpan)
{
    vec3 rgb = decodeToNits(texture(tex, uv), ref, sourceWhite);

    if (spatialHighlightRecovery > 0.0 && texSize.x > 0 && texSize.y > 0) {
        rgb = reconstructHighlightsSpatial(tex, uv, rgb, ref, texSize, sourceWhite, spatialHighlightRecovery,
                                           captureUsesFloat);
    } else {
        rgb = reconstructHighlights(rgb, ref);
    }

    float lumaNits = max(dot(rgb, AUTOHDR_LUMA), 1e-6);
    float t = lumaNits / ref;
    t = adaptiveShadowRolloff(t, minDisplayNits, ref);
    t = applyUserBlackPoint(t, blackPoint);

    float correctedNits;
    float Yn = applyToneCurveMapping(t, ref, curveSpan, correctedNits);
    rgb = applyICtCpToneCurve(rgb, Yn, lumaNits, ref);
    rgb = autohdrApplyChromaCompensation(rgb, Yn, lumaNits, chromaCompensation);

    if (gamutExpansion > 0.0) {
        rgb = expandGamutSmart(rgb / ref, gamutExpansion) * ref;
        rgb = mapToDisplayGamutICtCp(rgb, gamutMappingStrength);
        float channelPeak = max(max(rgb.r, rgb.g), rgb.b);
        if (channelPeak > displayPeak) {
            rgb *= displayPeak / channelPeak;
        }
    }

    vec3 sceneMapped = applyColorControls(rgb / ref, 1.0, colorVibrance);
    rgb = sceneMapped * ref;

    float outLuma = dot(rgb, AUTOHDR_LUMA);
    if (highlightRolloff <= 0.0) {
        if (outLuma > displayPeak) {
            rgb *= displayPeak / outLuma;
        }
    } else {
        float kneeNits = displayPeak * 0.85;
        rgb = compressHighlightsICtCp(rgb, kneeNits, displayPeak, highlightRolloff);
    }

    return rgb;
}

vec3 debandPostCurveLuma(sampler2D tex, vec2 texcoord, vec3 centerNits, float strength, ivec2 texSize, float ref,
                         float displayPeak, float sourceWhite, float curveSpan)
{
    if (strength <= 0.0 || texSize.x <= 0 || texSize.y <= 0) {
        return centerNits;
    }

    float centerLuma = dot(centerNits / ref, AUTOHDR_LUMA);
    if (centerLuma < 1e-6) {
        return centerNits;
    }

    ivec2 px = ivec2(clamp(texcoord * vec2(texSize), vec2(0.0), vec2(texSize - ivec2(1))));
    float neighborLumaAccum = 0.0;
    float gradAccum = 0.0;

    const ivec2 offsets[4] = ivec2[4](
        ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1)
    );

    for (int i = 0; i < 4; ++i) {
        ivec2 npx = clamp(px + offsets[i], ivec2(0), texSize - ivec2(1));
        vec2 nuv = (vec2(npx) + 0.5) / vec2(texSize);
        vec3 neighborNits = processPixelToOutput(tex, nuv, texSize, ref, displayPeak, sourceWhite, curveSpan);
        float neighborLuma = dot(neighborNits / ref, AUTOHDR_LUMA);
        neighborLumaAccum += neighborLuma;
        gradAccum += abs(neighborLuma - centerLuma);
    }

    float avgNeighborLuma = neighborLumaAccum * 0.25;
    float localGrad = gradAccum * 0.25;
    float flatness = 1.0 - smoothstep(0.001, 0.01, localGrad);
    float targetLuma = mix(centerLuma, avgNeighborLuma, flatness * strength);
    targetLuma = min(targetLuma, centerLuma);

    return centerNits * (targetLuma / centerLuma);
}

void main()
{
    vec4 tex = texture(sampler, texcoord0);

    float ref = max(destinationReferenceLuminance, 1.0);
    float displayPeak = max(maxDestinationLuminance, ref);
    float sourceWhite = max(sourceTransferFunctionParams.x + sourceTransferFunctionParams.y, 1.0);
    float curveSpan = toneCurveInputSpan > 1.0 ? toneCurveInputSpan : ref;
    ivec2 texSize = ivec2(textureWidth, textureHeight);

    float alpha = max(tex.a, 0.001);
    vec3 rgb = processPixelToOutput(sampler, texcoord0, texSize, ref, displayPeak, sourceWhite, curveSpan);

    float postDeband = postCurveDebandStrength;
    if (postDeband > 0.0 && texSize.x > 0 && texSize.y > 0) {
        rgb = debandPostCurveLuma(sampler, texcoord0, rgb, postDeband, texSize, ref, displayPeak, sourceWhite,
                                  curveSpan);
    }

    rgb = luminanceScaledDither(rgb, gl_FragCoord.xy, sampler, texcoord0, texSize, ditherStrength, ref, sourceWhite);

    tex.rgb = max(rgb * alpha, vec3(0.0));
    tex *= modulation;
    fragColor = nitsToDestinationEncoding(tex);
}
