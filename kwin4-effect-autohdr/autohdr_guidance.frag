#version 140

// ONNX guidance_v1 detail formula (keep in sync with tools/export_guidance_v1.py
// and ai/guidance_inference.cpp runCpuGuidance):
//   R = highlight expansion (>=1)
//   G = highlight confidence / shoulder-detail mask [0,1]
//   B = shadow detail mask [0,1]
//   A = depth / local-contrast mask [0,1]

uniform sampler2D sampler;
uniform int textureWidth;
uniform int textureHeight;

in vec2 texcoord0;
out vec4 fragColor;

const vec3 LUMA_W = vec3(0.2126, 0.7152, 0.0722);
const float INV_255 = 1.0 / 255.0;

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

float quantFlat(float delta)
{
    float d = abs(delta);
    return 1.0 - smoothstep(INV_255 + 1.0e-4, INV_255 * 2.0, d);
}

void main()
{
    ivec2 texSize = ivec2(max(textureWidth, 1), max(textureHeight, 1));
    vec2 texel = 1.0 / vec2(texSize);

    vec3 center = texture(sampler, texcoord0).rgb;
    float centerLuma = lumaOf(center);
    float centerSat = satOf(center);
    float peak = max(center.r, max(center.g, center.b));
    float minChannel = min(center.r, min(center.g, center.b));

    // 4-tap + diagonals for gradient / variance / quantization flatness.
    vec2 offsets[8] = vec2[8](
        vec2(texel.x, 0.0), vec2(-texel.x, 0.0),
        vec2(0.0, texel.y), vec2(0.0, -texel.y),
        vec2(texel.x, texel.y), vec2(-texel.x, texel.y),
        vec2(texel.x, -texel.y), vec2(-texel.x, -texel.y)
    );

    float grad = 0.0;
    float varAccum = 0.0;
    float flatAccum = 0.0;
    float localMax = centerLuma;
    for (int i = 0; i < 8; ++i) {
        float nLuma = lumaOf(texture(sampler, texcoord0 + offsets[i]).rgb);
        float d = nLuma - centerLuma;
        grad += abs(d);
        varAccum += d * d;
        flatAccum += quantFlat(d);
        localMax = max(localMax, nLuma);
    }
    grad *= 0.125;
    float variance = varAccum * 0.125;
    float flatness = flatAccum * 0.125;

    // --- Highlight expansion / confidence (v0 formula + shoulder flatness) ---
    float highlightConf = clamp((centerLuma - 0.55) / 0.40, 0.0, 1.0);
    float highlightLift = clamp((centerLuma - 0.45) / 0.53, 0.0, 1.0);
    float shoulderBand = smoothstep(0.85, 0.95, centerLuma);
    float shoulderDetail = shoulderBand * flatness;
    float highlightFine = max(highlightConf, shoulderDetail * 0.85);
    float shoulderFine = shoulderBand * flatness * 1.1;
    float confidence = max(highlightFine, shoulderFine);
    float expansion = 1.0 + confidence * highlightLift * 0.75;

    // --- Shadow detail mask (core + transition band) ---
    float shadowCore = 1.0 - smoothstep(0.0, 0.14, centerLuma);
    float shadowTrans = (1.0 - smoothstep(0.14, 0.24, centerLuma)) * 0.45;
    float shadowRegion = max(shadowCore, shadowTrans);
    float lowVar = 1.0 - smoothstep(0.0, 0.0025, variance);
    float softEdge = 1.0 - smoothstep(0.02, 0.10, grad);
    float mildGrad = smoothstep(0.004, 0.02, grad) * (1.0 - smoothstep(0.06, 0.16, grad));
    float shadowSoft = mix(1.0, softEdge, 0.35);
    float shadowMask = shadowRegion * max(flatness, max(lowVar * 0.85, mildGrad * 1.15)) * shadowSoft;

    // --- Depth / local-contrast mask (prefer quantized ramps over noisy skin) ---
    float midRegion = smoothstep(0.10, 0.20, centerLuma) * (1.0 - smoothstep(0.72, 0.88, centerLuma));
    float mildVar = smoothstep(0.0, 0.0008, variance) * (1.0 - smoothstep(0.003, 0.012, variance));
    float depthMask = midRegion * max(mildVar, mildGrad * 0.5) * softEdge * 0.65;

    // --- UI suppress ---
    // flatPanel matches sky/specular interiors as well as UI chrome; do NOT apply it to
    // highlight R/G or cores stay flat while only edges punch toward peak.
    float flatPanel = (1.0 - smoothstep(0.005, 0.035, grad))
        * clamp((centerLuma - 0.75) / 0.23, 0.0, 1.0)
        * (1.0 - clamp((centerSat - 0.05) / 0.25, 0.0, 1.0));
    float textEdge = clamp((grad - 0.08) / 0.17, 0.0, 1.0)
        * clamp((centerLuma - 0.4) / 0.5, 0.0, 1.0);
    float textKeep = 1.0 - textEdge * 0.85;
    float panelKeep = 1.0 - flatPanel;
    float detailKeep = panelKeep * textKeep;

    // Highlights: text-edge suppress only (restore v0-style punch on flat skies/speculars).
    confidence *= textKeep;
    expansion = mix(1.0, expansion, textKeep);

    shadowMask *= detailKeep;
    depthMask *= detailKeep;

    fragColor = vec4(
        clamp(expansion, 1.0, 1.75),
        clamp(confidence, 0.0, 1.0),
        clamp(shadowMask, 0.0, 1.0),
        clamp(depthMask, 0.0, 1.0));
}
