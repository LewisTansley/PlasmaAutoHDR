// AutoHDR perceptual color processing helpers (included into autohdr.frag)

const vec3 AUTOHDR_LUMA = vec3(0.2126, 0.7152, 0.0722);

// ICtCp conversions use KWin colormanagement.glsl uniforms (destinationToLMS, lmsToDestination)
// and constants (toICtCp, fromICtCp, linearToPq, pqToLinear) — same path as doTonemapping().

vec3 linearToICtCp(vec3 rgbNits)
{
    vec3 lms = (destinationToLMS * vec4(rgbNits, 1.0)).rgb;
    vec3 lmsPQ = linearToPq(lms / 10000.0);
    return toICtCp * lmsPQ;
}

vec3 iCtCpToLinear(vec3 ictcp)
{
    return (lmsToDestination * vec4(pqToLinear(fromICtCp * ictcp), 1.0)).rgb * 10000.0;
}

float adaptiveShadowRolloff(float t, float minDisplayNits, float refNits)
{
    float ref = max(refNits, 1.0);
    float displayFloor = max(minDisplayNits / ref, 0.0);
    float sdrToe = 5.0 / 255.0;
    float toeEnd = max(sdrToe, displayFloor);
    float u = clamp(t / toeEnd, 0.0, 1.0);
    float toe = toeEnd * u * u * (3.0 - 2.0 * u);
    return mix(toe, t, step(toeEnd + 1e-6, t));
}

float applyUserBlackPoint(float t, float offset)
{
    return max(t - offset, 0.0) / max(1.0 - offset, 1e-6);
}

vec3 applyICtCpToneCurve(vec3 rgbNits, float targetLumaNits, float sourceLumaNits, float refNits)
{
    if (abs(targetLumaNits - sourceLumaNits) < 1e-4) {
        return rgbNits;
    }

    vec3 ictcp = linearToICtCp(rgbNits);
    float IgrayIn = linearToICtCp(vec3(sourceLumaNits)).x;
    float IgrayOut = linearToICtCp(vec3(targetLumaNits)).x;
    ictcp.x += (IgrayOut - IgrayIn);

    vec3 result = max(iCtCpToLinear(ictcp), vec3(0.0));
    float peak = max(max(result.r, result.g), result.b);
    float maxAllowed = max(targetLumaNits, sourceLumaNits);
    if (peak > maxAllowed) {
        result *= maxAllowed / peak;
    }
    return result;
}

vec3 autohdrApplyChromaCompensation(vec3 rgbNits, float targetLumaNits, float sourceLumaNits, float strength)
{
    if (strength <= 0.0) {
        return rgbNits;
    }

    float lumaRatio = targetLumaNits / max(sourceLumaNits, 1e-4);
    if (lumaRatio <= 1.0) {
        return rgbNits;
    }

    vec3 ictcp = linearToICtCp(rgbNits);
    float highlightFalloff = 1.0 - smoothstep(1.0, 2.5, lumaRatio);
    float chromaBoost = 1.0 + 0.2 * strength * (lumaRatio - 1.0) * highlightFalloff;
    ictcp.yz *= chromaBoost;
    return max(iCtCpToLinear(ictcp), vec3(0.0));
}

vec3 compressHighlightsICtCp(vec3 rgbNits, float kneeNits, float peakNits, float rolloff)
{
    if (rolloff <= 0.0) {
        return rgbNits;
    }

    float luma = dot(rgbNits, AUTOHDR_LUMA);
    float blend = smoothstep(kneeNits * 0.95, kneeNits * 1.05, luma);
    if (blend <= 0.0) {
        return rgbNits;
    }

    vec3 compressed = rgbNits;
    if (luma > kneeNits) {
        vec3 ictcp = linearToICtCp(rgbNits);
        float Igray = linearToICtCp(vec3(luma)).x;
        float IgrayKnee = linearToICtCp(vec3(kneeNits)).x;
        float IgrayPeak = linearToICtCp(vec3(peakNits)).x;

        if (Igray > IgrayKnee && IgrayPeak > IgrayKnee) {
            float excess = (Igray - IgrayKnee) / max(IgrayPeak - IgrayKnee, 1e-4);
            float rolled = excess / (1.0 + rolloff * 3.0 * excess);
            float IgrayOut = IgrayKnee + rolled * (IgrayPeak - IgrayKnee);
            ictcp.x += (IgrayOut - Igray);
            compressed = max(iCtCpToLinear(ictcp), vec3(0.0));
        }
    }

    return mix(rgbNits, compressed, blend);
}

vec3 mapToDisplayGamutICtCp(vec3 rgbNits, float strength)
{
    if (strength <= 0.0) {
        return rgbNits;
    }

    vec3 ictcp = linearToICtCp(rgbNits);
    float chroma = length(ictcp.yz);
    if (chroma <= 1e-4) {
        return rgbNits;
    }

    const float maxChroma = 0.5;
    if (chroma <= maxChroma) {
        return rgbNits;
    }

    float scale = maxChroma / chroma;
    float mapped = mix(1.0, scale, smoothstep(maxChroma, maxChroma * 1.5, chroma) * strength);
    ictcp.yz *= mapped;
    return max(iCtCpToLinear(ictcp), vec3(0.0));
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

vec3 reconstructHighlightsSpatial(sampler2D tex, vec2 texcoord, vec3 rgbNits, float refNits, ivec2 texSize,
                                  float sourceWhite, float strength, int captureUsesFloat)
{
    vec3 base = reconstructHighlights(rgbNits, refNits);
    if (strength <= 0.0 || texSize.x <= 0 || texSize.y <= 0) {
        return base;
    }

    vec3 rel = rgbNits / max(refNits, 1.0);
    float peak = max(max(rel.r, rel.g), rel.b);
    if (peak <= 0.98) {
        return base;
    }

    ivec2 px = ivec2(clamp(texcoord * vec2(texSize), vec2(0.0), vec2(texSize - ivec2(1))));
    vec3 hueAccum = vec3(0.0);
    float weight = 0.0;

    const ivec2 offsets[8] = ivec2[8](
        ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1),
        ivec2(1, 1), ivec2(-1, 1), ivec2(1, -1), ivec2(-1, -1)
    );

    for (int i = 0; i < 8; ++i) {
        ivec2 npx = clamp(px + offsets[i], ivec2(0), texSize - ivec2(1));
        vec2 nuv = (vec2(npx) + 0.5) / vec2(texSize);
        vec4 nSample = texture(tex, nuv);
        if (captureUsesFloat == 0) {
            nSample = clamp(nSample, 0.0, 1.0);
        }
        nSample = encodingToNits(nSample, sourceNamedTransferFunction, sourceTransferFunctionParams.x,
                               sourceTransferFunctionParams.y);
        nSample.rgb = (colorimetryTransform * vec4(nSample.rgb, 1.0)).rgb;
        nSample.rgb *= refNits / max(sourceWhite, 1.0);
        vec3 nRel = nSample.rgb / max(nSample.a, 0.001) / refNits;
        float nPeak = max(max(nRel.r, nRel.g), nRel.b);
        if (nPeak < 0.95) {
            vec3 hue = nRel / max(nPeak, 1e-4);
            hueAccum += hue;
            weight += 1.0;
        }
    }

    if (weight <= 0.0) {
        return base;
    }

    vec3 avgHue = hueAccum / weight;
    float luma = dot(rel, AUTOHDR_LUMA);
    vec3 spatial = avgHue * luma;
    float spatialPeak = max(max(spatial.r, spatial.g), spatial.b);
    if (spatialPeak > peak) {
        spatial *= peak / max(spatialPeak, 1e-6);
    }
    float spatialLuma = dot(spatial, AUTOHDR_LUMA);
    if (peak > 0.95 && spatialLuma > luma) {
        spatial *= luma / max(spatialLuma, 1e-6);
    }
    float clipMask = smoothstep(0.95, 0.99, peak);
    return mix(base, spatial * refNits, clipMask * strength * 0.5);
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

float isBandStep(float delta)
{
    const float s = 1.0 / 255.0;
    float d = abs(delta);
    return step(s - 2.0e-4, d) * (1.0 - step(s + 2.0e-4, d));
}

vec3 debandNeighbors(vec3 centerRel, vec3 neighborRel, float strength)
{
    float band = isBandStep(neighborRel.r - centerRel.r)
               * isBandStep(neighborRel.g - centerRel.g)
               * isBandStep(neighborRel.b - centerRel.b);
    return mix(centerRel, (centerRel + neighborRel) * 0.5, band * strength);
}

float ign(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

float triangularIgn(vec2 p)
{
    return (ign(p) + ign(p + vec2(17.0, 89.0)) - 1.0) * 0.5;
}

float decodeInputRelLuma(sampler2D tex, vec2 uv, float ref, float sourceWhite)
{
    vec4 texSample = texture(tex, uv);
    texSample = encodingToNits(texSample, sourceNamedTransferFunction, sourceTransferFunctionParams.x,
                               sourceTransferFunctionParams.y);
    texSample.rgb = (colorimetryTransform * vec4(texSample.rgb, 1.0)).rgb;
    texSample.rgb *= ref / sourceWhite;
    float alpha = max(texSample.a, 0.001);
    return dot(texSample.rgb / alpha / ref, AUTOHDR_LUMA);
}

float inputLumaFlatness(sampler2D tex, vec2 texcoord, ivec2 texSize, float ref, float sourceWhite)
{
    if (texSize.x <= 0 || texSize.y <= 0) {
        return 0.0;
    }

    ivec2 px = ivec2(clamp(texcoord * vec2(texSize), vec2(0.0), vec2(texSize - ivec2(1))));
    float centerLuma = decodeInputRelLuma(tex, texcoord, ref, sourceWhite);
    float gradAccum = 0.0;

    const ivec2 offsets[4] = ivec2[4](
        ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1)
    );

    for (int i = 0; i < 4; ++i) {
        ivec2 npx = clamp(px + offsets[i], ivec2(0), texSize - ivec2(1));
        vec2 nuv = (vec2(npx) + 0.5) / vec2(texSize);
        gradAccum += abs(decodeInputRelLuma(tex, nuv, ref, sourceWhite) - centerLuma);
    }

    float localGrad = gradAccum * 0.25;
    return 1.0 - smoothstep(0.001, 0.01, localGrad);
}

vec3 luminanceScaledDither(vec3 rgbNits, vec2 px, sampler2D tex, vec2 texcoord, ivec2 texSize, float strength,
                           float refNits, float sourceWhite)
{
    if (strength <= 0.0) {
        return rgbNits;
    }

    float ref = max(refNits, 1.0);
    vec3 rel = rgbNits / ref;
    float outLuma = dot(rel, AUTOHDR_LUMA);
    float shadowW = 1.0 - smoothstep(0.0, 0.05, outLuma);
    float highlightW = smoothstep(0.4, 1.0, outLuma);
    float flatness = inputLumaFlatness(tex, texcoord, texSize, ref, sourceWhite);
    float amp = strength * flatness * mix(0.15, 0.4, highlightW) * mix(1.0, 0.1, shadowW);
    if (amp <= 0.0) {
        return rgbNits;
    }

    vec3 ictcp = linearToICtCp(rgbNits);
    ictcp.x += (triangularIgn(px) - 0.5) * amp;
    return max(iCtCpToLinear(ictcp), vec3(0.0));
}
