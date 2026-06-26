#version 450

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D inputImage;

layout(set = 0, binding = 1) uniform ToneCurveBlock {
    float toneCurveLut[256];
};

layout(push_constant) uniform PushConstants {
    float blackPoint;
    float colorVibrance;
    float gamutExpansion;
    float referenceNits;
    float displayPeak;
    float toneCurveInputSpan;
    int colorMode; // 0=SDR sRGB, 1=scRGB, 2=PQ
} pc;

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

float srgbToLinear(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

vec3 srgbToLinear(vec3 c)
{
    return vec3(srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b));
}

float linearToSrgb(float c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

vec3 linearToSrgb(vec3 c)
{
    return clamp(vec3(linearToSrgb(c.r), linearToSrgb(c.g), linearToSrgb(c.b)), 0.0, 1.0);
}

float pqEotf(float n)
{
    const float m1 = 2610.0 / 4096.0 / 4.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float np = pow(max(n, 0.0), 1.0 / m2);
    float l = max(np - c1, 0.0) / max(c2 - c3 * np, 1e-6);
    return pow(l, 1.0 / m1) * 10000.0;
}

float pqOetf(float nits)
{
    const float m1 = 2610.0 / 4096.0 / 4.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float y = pow(clamp(nits / 10000.0, 0.0, 1.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

vec3 decodeToNits(vec3 rgb)
{
    if (pc.colorMode == 1) {
        return rgb * 80.0;
    }
    if (pc.colorMode == 2) {
        return vec3(pqEotf(rgb.r), pqEotf(rgb.g), pqEotf(rgb.b));
    }
    vec3 linear = srgbToLinear(rgb);
    return linear * pc.referenceNits;
}

vec3 encodeFromNits(vec3 nits)
{
    if (pc.colorMode == 1) {
        return clamp(nits / 80.0, 0.0, 1.0);
    }
    if (pc.colorMode == 2) {
        return vec3(pqOetf(nits.r), pqOetf(nits.g), pqOetf(nits.b));
    }
    vec3 linear = nits / max(pc.referenceNits, 1.0);
    return linearToSrgb(linear);
}

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
    float luma = dot(rgb, LUMA);
    float maxVal = max(rgb.r, max(rgb.g, rgb.b));
    float minVal = min(rgb.r, min(rgb.g, rgb.b));
    float lnm = maxVal - minVal;

    if (abs(vib) > 0.001) {
        float amt = vib * (1.0 - lnm);
        rgb = mix(vec3(luma), rgb, 1.0 + amt);
    }
    return mix(vec3(luma), rgb, sat);
}

vec3 rec709ToXYZ(vec3 linearRec709)
{
    return vec3(
        dot(linearRec709, vec3(0.412390798, 0.357584327, 0.180480793)),
        dot(linearRec709, vec3(0.212639004, 0.715168655, 0.072192319)),
        dot(linearRec709, vec3(0.019330818, 0.119194783, 0.950532138))
    );
}

vec3 ap1D65ToRec709(vec3 linearAP1)
{
    const mat3 ap1D65ToXYZ = mat3(
        vec3(0.647507191, 0.266086400, -0.005448868),
        vec3(0.134379134, 0.675967813,  0.004072095),
        vec3(0.168569595, 0.057945795,  1.090434551)
    );
    const mat3 xyzToRec709 = mat3(
        vec3(3.240969896, -0.969243646,  0.055630080),
        vec3(-1.537383198,  1.875967503, -0.203976959),
        vec3(-0.498610765,  0.041555058,  1.056971550)
    );
    return xyzToRec709 * (ap1D65ToXYZ * linearAP1);
}

vec3 expandGamut(vec3 vHDRColor, float fExpandGamut)
{
    if (fExpandGamut <= 0.0) {
        return vHDRColor;
    }

    const mat3 sRGB_2_AP1_D65 = mat3(
        vec3(0.616850994, 0.069866394, 0.020549067),
        vec3(0.334062934, 0.917416679, 0.107642211),
        vec3(0.049086072, 0.012716927, 0.871808722)
    );
    const mat3 AP1_D65_2_sRGB = mat3(
        vec3(1.692679398, -0.128573980, -0.024022465),
        vec3(-0.606218057,  1.137933633, -0.126211718),
        vec3(-0.086461341, -0.009359653,  1.150234183)
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

    float LumaAP1 = rec709ToXYZ(ap1D65ToRec709(ColorAP1)).y;
    vec3 ChromaAP1 = ColorAP1 / max(LumaAP1, 1e-6);

    float ChromaDistSqr = dot(ChromaAP1 - 1.0, ChromaAP1 - 1.0);
    ChromaDistSqr = max(abs(ChromaDistSqr), 0.000001);

    float ExpandAmount = (1.0 - exp2(-4.0 * ChromaDistSqr))
                       * (1.0 - exp2(-4.0 * fExpandGamut * LumaAP1 * LumaAP1));

    vec3 ColorExpand = ExpandMat * ColorAP1;
    ColorAP1 = mix(ColorAP1, ColorExpand, ExpandAmount);

    return AP1_2_sRGB * ColorAP1;
}

void main()
{
    vec4 tex = texture(inputImage, vTexCoord);
    vec3 rgb = decodeToNits(tex.rgb);

    float ref = max(pc.referenceNits, 1.0);
    float displayPeak = max(pc.displayPeak, ref);

    float lumaNits = max(dot(rgb, LUMA), 1e-6);
    float t = lumaNits / ref;
    t = max(t - pc.blackPoint, 0.0) / max(1.0 - pc.blackPoint, 1e-6);

    float curveSpan = pc.toneCurveInputSpan > 1.0 ? pc.toneCurveInputSpan : ref;
    float correctedNits = t * ref;
    float Yn = mapToneCurve(correctedNits, curveSpan);
    rgb = rgb * (Yn / lumaNits);

    vec3 sceneMapped = applyColorControls(rgb / ref, 1.0, pc.colorVibrance);
    rgb = sceneMapped * ref;

    if (pc.gamutExpansion > 0.0) {
        rgb = expandGamut(rgb / ref, pc.gamutExpansion) * ref;
    }

    float outLuma = dot(rgb, LUMA);
    if (outLuma > displayPeak) {
        rgb *= displayPeak / outLuma;
    }

    outColor = vec4(encodeFromNits(max(rgb, vec3(0.0))), tex.a);
}
