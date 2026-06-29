#version 140

#include "colormanagement.glsl"

uniform sampler2D sampler;
uniform vec4 modulation;

uniform float blackPoint;
uniform float colorVibrance;
uniform float gamutExpansion;
uniform float highlightRolloff;
uniform float chromaCompensation;
uniform float toneCurveInputSpan;
uniform float toneCurveReferenceNits;
uniform float minDisplayNits;
uniform float toneCurveLut[512];
uniform float debandStrength;
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
    float ref = max(min(referenceNits, span), 1e-3);
    inputNits = clamp(inputNits, 0.0, span);

    if (inputNits <= ref) {
        return (inputNits / ref) * 0.5;
    }

    float logRef = log(ref);
    float logSpan = log(span);
    float t = (log(inputNits) - logRef) / max(logSpan - logRef, 1e-4);
    return 0.5 + t * 0.5;
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
    texSample = clamp(texSample, 0.0, 1.0);
    float alpha = max(texSample.a, 0.001);
    texSample = encodingToNits(texSample, sourceNamedTransferFunction, sourceTransferFunctionParams.x,
                             sourceTransferFunctionParams.y);
    texSample.rgb = (colorimetryTransform * vec4(texSample.rgb, 1.0)).rgb;
    texSample.rgb *= ref / sourceWhite;
    return texSample.rgb / alpha / ref;
}

vec3 debandSdrRel(sampler2D tex, vec2 texcoord, vec3 centerRel, float strength, ivec2 texSize, float ref,
                  float sourceWhite)
{
    if (strength <= 0.0) {
        return centerRel;
    }

    ivec2 px = ivec2(clamp(texcoord * vec2(texSize), vec2(0.0), vec2(texSize - ivec2(1))));
    vec3 accum = centerRel;
    float count = 1.0;

    const ivec2 offsets[4] = ivec2[4](
        ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1)
    );

    for (int i = 0; i < 4; ++i) {
        ivec2 npx = clamp(px + offsets[i], ivec2(0), texSize - ivec2(1));
        vec2 nuv = (vec2(npx) + 0.5) / vec2(texSize);
        vec3 neighborRel = sampleSdrRel(tex, nuv, ref, sourceWhite);
        float band = isBandStep(neighborRel.r - centerRel.r)
                   * isBandStep(neighborRel.g - centerRel.g)
                   * isBandStep(neighborRel.b - centerRel.b);
        accum += mix(centerRel, (centerRel + neighborRel) * 0.5, band * strength);
        count += band * strength;
    }

    return accum / count;
}

void main()
{
    vec4 tex = texture(sampler, texcoord0);
    tex = clamp(tex, 0.0, 1.0);

    tex = encodingToNits(tex, sourceNamedTransferFunction, sourceTransferFunctionParams.x, sourceTransferFunctionParams.y);
    tex.rgb = (colorimetryTransform * vec4(tex.rgb, 1.0)).rgb;

    float ref = max(destinationReferenceLuminance, 1.0);
    float displayPeak = max(maxDestinationLuminance, ref);
    float curveRef = toneCurveReferenceNits > 1.0 ? toneCurveReferenceNits : ref;

    float sourceWhite = max(sourceTransferFunctionParams.x + sourceTransferFunctionParams.y, 1.0);
    tex.rgb *= ref / sourceWhite;

    float alpha = max(tex.a, 0.001);
    vec3 rgb = tex.rgb / alpha;

    float deband = processingQuality > 0 ? debandStrength : 0.0;
    ivec2 texSize = ivec2(textureWidth, textureHeight);
    if (deband > 0.0 && texSize.x > 0 && texSize.y > 0) {
        vec3 centerRel = rgb / ref;
        centerRel = debandSdrRel(sampler, texcoord0, centerRel, deband, texSize, ref, sourceWhite);
        rgb = centerRel * ref;
    }

    if (processingQuality >= 2 && texSize.x > 0 && texSize.y > 0) {
        rgb = reconstructHighlightsSpatial(sampler, texcoord0, rgb, ref, texSize, sourceWhite, processingQuality);
    } else {
        rgb = reconstructHighlights(rgb, ref);
    }

    float lumaNits = max(dot(rgb, AUTOHDR_LUMA), 1e-6);
    float t = lumaNits / ref;
    t = adaptiveShadowRolloff(t, minDisplayNits, ref);
    t = applyUserBlackPoint(t, blackPoint);

    float curveSpan = toneCurveInputSpan > 1.0 ? toneCurveInputSpan : curveRef;
    float curveScale = curveRef / max(ref, 1e-3);
    float correctedNits = lumaNits * curveScale;
    float Yn = mapToneCurve(correctedNits, curveSpan, curveRef);
    rgb = applyICtCpToneCurve(rgb, Yn, lumaNits, ref, chromaCompensation);

    if (gamutExpansion > 0.0) {
        rgb = expandGamutSmart(rgb / ref, gamutExpansion) * ref;
        rgb = mapToDisplayGamutICtCp(rgb);
    }

    vec3 sceneMapped = applyColorControls(rgb / ref, 1.0, colorVibrance);
    rgb = sceneMapped * ref;

    float kneeNits = displayPeak * 0.85;
    rgb = compressHighlightsICtCp(rgb, kneeNits, displayPeak, highlightRolloff);

    rgb = luminanceScaledDither(rgb, gl_FragCoord.xy, ditherStrength, ref, processingQuality);

    tex.rgb = max(rgb * alpha, vec3(0.0));
    tex *= modulation;
    fragColor = nitsToDestinationEncoding(tex);
}
