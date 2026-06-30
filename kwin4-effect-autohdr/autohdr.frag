#version 140

#include "colormanagement.glsl"

uniform sampler2D sampler;
uniform vec4 modulation;

uniform float blackPoint;
uniform float colorVibrance;
uniform float gamutExpansion;
uniform float toneCurveInputSpan;
uniform float toneCurveLut[256];
uniform float debandStrength;
uniform float ditherStrength;
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
    float idx = u * 255.0;
    int i0 = int(floor(idx));
    int i1 = min(i0 + 1, 255);
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
    float alpha = max(texSample.a, 0.001);
    texSample = encodingToNits(texSample, sourceNamedTransferFunction, sourceTransferFunctionParams.x,
                             sourceTransferFunctionParams.y);
    texSample.rgb = (colorimetryTransform * vec4(texSample.rgb, 1.0)).rgb;
    texSample.rgb *= ref / sourceWhite;
    return texSample.rgb / alpha / ref;
}

vec3 spatialAvgPreCurve(sampler2D tex, vec2 texcoord, vec3 centerRel, float strength, ivec2 texSize, float ref,
                        float sourceWhite)
{
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
        vec3 neighborRel = sampleSdrRel(tex, nuv, ref, sourceWhite);
        float inputGrad = abs(dot(neighborRel, AUTOHDR_LUMA) - centerLuma);
        float flatW = 1.0 - smoothstep(0.001, 0.008, inputGrad);
        float rgbFlat = quantFlatness(neighborRel.r - centerRel.r)
                      * quantFlatness(neighborRel.g - centerRel.g)
                      * quantFlatness(neighborRel.b - centerRel.b);
        float w = rgbFlat * regionW * flatW * strength;
        accum += mix(centerRel, (centerRel + neighborRel) * 0.5, w);
        count += w;
    }

    return accum / count;
}

vec3 decodeRgbNits(vec4 tex, float ref, float sourceWhite)
{
    tex = encodingToNits(tex, sourceNamedTransferFunction, sourceTransferFunctionParams.x,
                         sourceTransferFunctionParams.y);
    tex.rgb = (colorimetryTransform * vec4(tex.rgb, 1.0)).rgb;
    tex.rgb *= ref / sourceWhite;
    float alpha = max(tex.a, 0.001);
    return tex.rgb / alpha;
}

vec3 toneMapPipeline(vec3 rgb, float ref, float displayPeak, float curveSpan)
{
    rgb = reconstructHighlights(rgb, ref);

    float lumaNits = max(dot(rgb, AUTOHDR_LUMA), 1e-6);
    float t = lumaNits / ref;
    t = applyUserBlackPoint(t, blackPoint);

    float inputNits = t * ref;
    float outputNits = mapToneCurve(inputNits, curveSpan);
    rgb *= outputNits / max(inputNits, 1e-6);

    if (gamutExpansion > 0.0) {
        rgb = expandGamutSmart(rgb / ref, gamutExpansion) * ref;
    }

    vec3 sceneMapped = applyColorControls(rgb / ref, 1.0, colorVibrance);
    rgb = sceneMapped * ref;

    float outLuma = dot(rgb, AUTOHDR_LUMA);
    if (outLuma > displayPeak) {
        rgb *= displayPeak / outLuma;
    }

    return rgb;
}

vec3 sampleToneMappedNits(sampler2D tex, vec2 uv, float ref, float displayPeak, float sourceWhite, float curveSpan)
{
    return toneMapPipeline(decodeRgbNits(texture(tex, uv), ref, sourceWhite), ref, displayPeak, curveSpan);
}

void main()
{
    vec4 tex = texture(sampler, texcoord0);

    float ref = max(destinationReferenceLuminance, 1.0);
    float displayPeak = max(maxDestinationLuminance, ref);
    float sourceWhite = max(sourceTransferFunctionParams.x + sourceTransferFunctionParams.y, 1.0);
    float curveSpan = toneCurveInputSpan > 1.0 ? toneCurveInputSpan : ref;

    vec3 rgb = decodeRgbNits(tex, ref, sourceWhite);

    float spatialAvg = processingQuality > 0 ? debandStrength : 0.0;
    if (spatialAvg > 0.0 && enableSpatialAvgPreCurve > 0 && textureWidth > 0 && textureHeight > 0) {
        ivec2 texSize = ivec2(textureWidth, textureHeight);
        vec3 centerRel = rgb / ref;
        centerRel = spatialAvgPreCurve(sampler, texcoord0, centerRel, spatialAvg, texSize, ref, sourceWhite);
        rgb = centerRel * ref;
    }

    rgb = toneMapPipeline(rgb, ref, displayPeak, curveSpan);

    float localGrad = 0.0;
    vec3 neighbor0 = rgb;
    vec3 neighbor1 = rgb;
    vec3 neighbor2 = rgb;
    vec3 neighbor3 = rgb;
    if ((spatialAvg > 0.0 || ditherStrength > 0.0) && textureWidth > 0 && textureHeight > 0) {
        ivec2 texSize = ivec2(textureWidth, textureHeight);
        ivec2 px = ivec2(clamp(texcoord0 * vec2(texSize), vec2(0.0), vec2(texSize - ivec2(1))));

        float centerLuma = dot(rgb / ref, AUTOHDR_LUMA);
        float gradAccum = 0.0;

        ivec2 npx0 = clamp(px + ivec2(1, 0), ivec2(0), texSize - ivec2(1));
        neighbor0 = sampleToneMappedNits(sampler, (vec2(npx0) + 0.5) / vec2(texSize), ref, displayPeak, sourceWhite, curveSpan);
        gradAccum += abs(dot(neighbor0 / ref, AUTOHDR_LUMA) - centerLuma);

        ivec2 npx1 = clamp(px + ivec2(-1, 0), ivec2(0), texSize - ivec2(1));
        neighbor1 = sampleToneMappedNits(sampler, (vec2(npx1) + 0.5) / vec2(texSize), ref, displayPeak, sourceWhite, curveSpan);
        gradAccum += abs(dot(neighbor1 / ref, AUTOHDR_LUMA) - centerLuma);

        ivec2 npx2 = clamp(px + ivec2(0, 1), ivec2(0), texSize - ivec2(1));
        neighbor2 = sampleToneMappedNits(sampler, (vec2(npx2) + 0.5) / vec2(texSize), ref, displayPeak, sourceWhite, curveSpan);
        gradAccum += abs(dot(neighbor2 / ref, AUTOHDR_LUMA) - centerLuma);

        ivec2 npx3 = clamp(px + ivec2(0, -1), ivec2(0), texSize - ivec2(1));
        neighbor3 = sampleToneMappedNits(sampler, (vec2(npx3) + 0.5) / vec2(texSize), ref, displayPeak, sourceWhite, curveSpan);
        gradAccum += abs(dot(neighbor3 / ref, AUTOHDR_LUMA) - centerLuma);

        localGrad = gradAccum * 0.25;
    }

    if (spatialAvg > 0.0) {
        rgb = spatialAvgPostCurve(rgb, neighbor0, neighbor1, neighbor2, neighbor3, spatialAvg, ref, localGrad);
    }

    rgb = luminanceScaledDither(rgb, gl_FragCoord.xy, ditherStrength, ref, localGrad, spatialAvg);

    float alpha = max(tex.a, 0.001);
    tex.rgb = max(rgb * alpha, vec3(0.0));
    tex *= modulation;
    fragColor = nitsToDestinationEncoding(tex);
}
