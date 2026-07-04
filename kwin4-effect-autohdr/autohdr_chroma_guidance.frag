#version 140

// Chroma guidance map (keep in sync with tools/export_chroma_v0.py and ai/chroma_inference.cpp):
//   R = chroma correction strength [0,1]
//   G = saturation residual hint [-1,1] (stored 0..1, remapped in main pass)
//   B = hue residual hint [-1,1] (stored 0..1, remapped in main pass)
//   A = per-pixel colorIntensity blend [0,1] for PQ remap

uniform sampler2D sampler;
uniform int textureWidth;
uniform int textureHeight;

in vec2 texcoord0;
out vec4 fragColor;

const vec3 LUMA_W = vec3(0.2126, 0.7152, 0.0722);
const float INV_255 = 1.0 / 255.0;

const mat3 SRGB_2_AP1 = mat3(
    vec3(0.616850994, 0.069866394, 0.020549067),
    vec3(0.334062934, 0.917416679, 0.107642211),
    vec3(0.049086072, 0.012716927, 0.871808722)
);

const mat3 AP1_TO_XYZ = mat3(
    vec3(0.647507191, 0.266086400, -0.005448868),
    vec3(0.134379134, 0.675967813, 0.004072095),
    vec3(0.168569595, 0.057945795, 1.090434551)
);

float lumaOf(vec3 rgb)
{
    return dot(max(rgb, vec3(0.0)), LUMA_W);
}

float satOf(vec3 rgb)
{
    float mx = max(rgb.r, max(rgb.g, rgb.b));
    float mn = min(rgb.r, min(rgb.g, rgb.b));
    return (mx - mn) / max(mx, 1e-4);
}

vec3 ap1ChromaOf(vec3 rgb)
{
    vec3 c = SRGB_2_AP1 * max(rgb, vec3(0.0));
    float y = max((AP1_TO_XYZ * c).y, 1e-6);
    return c / y;
}

float quantFlatChroma(float delta)
{
    float d = abs(delta);
    return 1.0 - smoothstep(INV_255 + 1.0e-4, INV_255 * 2.0, d);
}

float remapSigned(float v)
{
    return v * 2.0 - 1.0;
}

void main()
{
    ivec2 texSize = ivec2(max(textureWidth, 1), max(textureHeight, 1));
    vec2 texel = 1.0 / vec2(texSize);

    vec3 center = texture(sampler, texcoord0).rgb;
    float centerLuma = lumaOf(center);
    float centerSat = satOf(center);
    vec3 centerChroma = ap1ChromaOf(center);

    vec2 offsets[8] = vec2[8](
        vec2(texel.x, 0.0), vec2(-texel.x, 0.0),
        vec2(0.0, texel.y), vec2(0.0, -texel.y),
        vec2(texel.x, texel.y), vec2(-texel.x, texel.y),
        vec2(texel.x, -texel.y), vec2(-texel.x, -texel.y)
    );

    float lumaGrad = 0.0;
    float chromaFlat = 0.0;
    float chromaVar = 0.0;
    vec3 chromaAccum = centerChroma;

    for (int i = 0; i < 8; ++i) {
        vec3 n = texture(sampler, texcoord0 + offsets[i]).rgb;
        float nLuma = lumaOf(n);
        lumaGrad += abs(nLuma - centerLuma);
        vec3 nChroma = ap1ChromaOf(n);
        vec3 cd = nChroma - centerChroma;
        chromaVar += dot(cd, cd);
        chromaFlat += quantFlatChroma(cd.r) * quantFlatChroma(cd.g) * quantFlatChroma(cd.b);
        chromaAccum += nChroma;
    }
    lumaGrad *= 0.125;
    chromaVar *= 0.125;
    chromaFlat *= 0.125;
    vec3 avgChroma = chromaAccum / 9.0;

    // --- Chroma banding mask (mid-luma ramps, low luma gradient) ---
    float midRegion = smoothstep(0.08, 0.18, centerLuma) * (1.0 - smoothstep(0.82, 0.92, centerLuma));
    float smoothSky = 1.0 - smoothstep(0.012, 0.06, lumaGrad);
    float lowChromaVar = 1.0 - smoothstep(0.0002, 0.002, chromaVar);
    float strength = midRegion * max(chromaFlat, lowChromaVar * 0.75) * smoothSky;

    // Saturation residual: nudge toward neighborhood when banding detected
    float satHint = clamp(length(avgChroma - vec3(1.0)) - length(centerChroma - vec3(1.0)), -0.5, 0.5);
    float satStored = satHint * 0.5 + 0.5;

    // Hue residual: small angle hint from avg chroma direction
    vec2 cDir = vec2(centerChroma.r - 1.0, centerChroma.g - 1.0);
    vec2 aDir = vec2(avgChroma.r - 1.0, avgChroma.g - 1.0);
    float hueHint = clamp(atan(aDir.y, aDir.x) - atan(cDir.y, cDir.x), -0.15, 0.15) / 0.15;
    float hueStored = hueHint * 0.5 + 0.5;

    // --- Per-pixel colorIntensity (Phase 3 heuristic) ---
    float textEdge = clamp((lumaGrad - 0.08) / 0.17, 0.0, 1.0) * clamp((centerLuma - 0.4) / 0.5, 0.0, 1.0);
    float neutralGray = 1.0 - smoothstep(0.02, 0.12, centerSat);
    float skinHue = smoothstep(0.02, 0.08, centerSat) * (1.0 - smoothstep(0.45, 0.65, centerSat));
    float skinGate = skinHue * (1.0 - smoothstep(0.15, 0.35, lumaGrad));
    float preserveChroma = max(max(textEdge * 0.85, neutralGray * 0.5), skinGate * 0.7);
    float localColorIntensity = mix(0.55, 0.15, preserveChroma);

    // Highlight band: allow chroma recovery near clip
    float highlightBand = smoothstep(0.88, 0.98, centerLuma);
    strength = max(strength, highlightBand * (1.0 - textEdge) * 0.65);

    // UI suppress
    strength *= 1.0 - textEdge * 0.9;

    fragColor = vec4(
        clamp(strength, 0.0, 1.0),
        clamp(satStored, 0.0, 1.0),
        clamp(hueStored, 0.0, 1.0),
        clamp(localColorIntensity, 0.0, 1.0));
}
