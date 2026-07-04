// AutoHDR chroma inference helpers (included into autohdr.frag)
// Corrections preserve photometric Y (CIE XYZ Y) after tone mapping.

const mat3 AUTOHDR_SRGB_2_AP1_D65 = mat3(
    vec3(0.616850994, 0.069866394, 0.020549067),
    vec3(0.334062934, 0.917416679, 0.107642211),
    vec3(0.049086072, 0.012716927, 0.871808722)
);

const mat3 AUTOHDR_AP1_2_SRGB = mat3(
    vec3(1.70505, -0.13026, -0.02400),
    vec3(-0.62179, 1.14080, -0.12897),
    vec3(-0.08326, -0.01055, 1.15297)
);

const mat3 AUTOHDR_AP1_D65_TO_XYZ = mat3(
    vec3(0.647507191, 0.266086400, -0.005448868),
    vec3(0.134379134, 0.675967813, 0.004072095),
    vec3(0.168569595, 0.057945795, 1.090434551)
);

float autohdrAp1LumaFromRgbNits(vec3 rgbNits)
{
    return max((AUTOHDR_AP1_D65_TO_XYZ * (AUTOHDR_SRGB_2_AP1_D65 * rgbNits)).y, 1e-6);
}

vec3 autohdrRgbRelToAp1Chroma(vec3 rgbRel)
{
    vec3 colorAp1 = AUTOHDR_SRGB_2_AP1_D65 * max(rgbRel, vec3(0.0));
    float lumaAp1 = max((AUTOHDR_AP1_D65_TO_XYZ * colorAp1).y, 1e-6);
    return colorAp1 / lumaAp1;
}

vec3 autohdrAp1ChromaToRgbNits(vec3 chromaAp1, vec3 rgbNitsReference)
{
    float lumaAp1 = autohdrAp1LumaFromRgbNits(rgbNitsReference);
    vec3 rgbOut = AUTOHDR_AP1_2_SRGB * (chromaAp1 * lumaAp1);
    float yBefore = max(luminanceYNits(rgbNitsReference), 1e-6);
    float yAfter = max(luminanceYNits(rgbOut), 1e-6);
    return max(rgbOut * (yBefore / yAfter), vec3(0.0));
}

vec3 autohdrApplyLogChromaDelta(vec3 rgbNits, vec3 chromaAp1, float deltaSat, float deltaHueRad)
{
    vec3 c = chromaAp1;
    vec2 rg = vec2(c.r - 1.0, c.g - 1.0);
    float sat = length(rg);
    float angle = atan(rg.y, rg.x);
    sat = max(sat * exp(deltaSat), 0.0);
    angle += deltaHueRad;
    vec2 dir = sat * vec2(cos(angle), sin(angle));
    c = vec3(1.0 + dir.x, 1.0 + dir.y, 1.0);
    c = max(c, vec3(1e-4));
    return autohdrAp1ChromaToRgbNits(c, rgbNits);
}

vec3 applyChromaDecontour(vec3 rgbNits, float refNits, vec4 chromaGuide, float strength,
                          vec3 wideChromaAp1, vec3 localChromaAp1)
{
    float w = clamp(chromaGuide.r, 0.0, 1.0) * clamp(strength, 0.0, 1.0);
    if (w <= 1e-4) {
        return rgbNits;
    }

    vec3 chromaAp1 = autohdrRgbRelToAp1Chroma(rgbNits / max(refNits, 1.0));
    vec3 delta = chromaAp1 - localChromaAp1;
    float satResidual = clamp(chromaGuide.g, -1.0, 1.0);
    vec3 target = localChromaAp1 + delta * (1.0 - w * 0.35);
    target = mix(target, wideChromaAp1, w * 0.45 * (0.5 + 0.5 * satResidual));
    return autohdrAp1ChromaToRgbNits(target, rgbNits);
}

vec3 applyHighlightChromaRecovery(vec3 rgbNits, float refNits, vec4 chromaGuide, vec4 lumaGuidance,
                                  float strength, vec3 neighborChromaAp1, float neighborWeight)
{
    float w = clamp(chromaGuide.r, 0.0, 1.0) * clamp(lumaGuidance.g, 0.0, 1.0) * clamp(strength, 0.0, 1.0);
    if (w <= 1e-4 || neighborWeight <= 1e-4) {
        return rgbNits;
    }

    vec3 rel = rgbNits / max(refNits, 1.0);
    float peak = max(max(rel.r, rel.g), rel.b);
    float minChannel = min(rel.r, min(rel.g, rel.b));
    float nearClip = step(0.98, peak);
    float channelSpread = (peak - minChannel) / max(peak, 1e-4);
    float clipMask = nearClip * step(0.05, channelSpread);
    if (clipMask <= 0.0) {
        return rgbNits;
    }

    vec3 chromaAp1 = autohdrRgbRelToAp1Chroma(rel);
    vec3 delta = neighborChromaAp1 - chromaAp1;
    float hueVar = length(delta);
    float consistentHue = 1.0 - smoothstep(0.08, 0.35, hueVar);
    float desat = smoothstep(0.25, 0.6, hueVar);

    vec3 target = mix(chromaAp1, neighborChromaAp1, consistentHue);
    target = mix(target, vec3(1.0), desat * 0.35);

    float hueResidual = clamp(chromaGuide.b, -1.0, 1.0) * 0.05;
    float satResidual = clamp(chromaGuide.g, -1.0, 1.0);
    vec3 rgbOut = autohdrApplyLogChromaDelta(rgbNits, target, satResidual * w * 0.15, hueResidual * w);
    rgbOut = mix(rgbNits, rgbOut, clipMask * w * neighborWeight);
    return autohdrAp1ChromaToRgbNits(autohdrRgbRelToAp1Chroma(rgbOut / max(refNits, 1.0)), rgbNits);
}

vec3 applyChromaRefinement(vec3 rgbNits, float refNits, vec4 chromaGuide, vec4 lumaGuidance, float strength,
                           vec3 wideChromaAp1, vec3 localChromaAp1, vec3 neighborChromaAp1, float neighborWeight)
{
    float w = clamp(chromaGuide.r, 0.0, 1.0) * clamp(strength, 0.0, 1.0);
    if (w <= 1e-4) {
        return rgbNits;
    }

    rgbNits = applyChromaDecontour(rgbNits, refNits, chromaGuide, strength, wideChromaAp1, localChromaAp1);

    float satResidual = clamp(chromaGuide.g, -1.0, 1.0);
    float hueResidual = clamp(chromaGuide.b, -1.0, 1.0);
    vec3 chromaAp1 = autohdrRgbRelToAp1Chroma(rgbNits / max(refNits, 1.0));
    rgbNits = autohdrApplyLogChromaDelta(rgbNits, chromaAp1, satResidual * w * 0.12, hueResidual * w * 0.04);

    rgbNits = applyHighlightChromaRecovery(rgbNits, refNits, chromaGuide, lumaGuidance, strength, neighborChromaAp1,
                                           neighborWeight);
    return rgbNits;
}
