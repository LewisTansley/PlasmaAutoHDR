#version 140

#include "colormanagement.glsl"

uniform sampler2D sampler;
uniform vec4 modulation;

uniform float maxNits;
uniform float blackPoint;
uniform float midPoint;
uniform float highlightExpansion;
uniform float highlightLift;
uniform float highlightRange;
uniform float colorVibrance;
uniform float gamutExpansion;

in vec2 texcoord0;
out vec4 fragColor;

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

// BT.2408-inspired reference white system: SDR white at paper white, smooth shoulder to peak.
float mapTone(float t, float paperWhiteNits, float peakNits, float highlightExp, float highlightLiftVal, float highlightRangeVal)
{
    t = max(t, 0.0);
    if (t <= 1.0) {
        return t * paperWhiteNits;
    }
    float headroom = max(peakNits - paperWhiteNits, 1e-3);
    float excess = t - 1.0;
    float knee = max(1.0, peakNits / paperWhiteNits - 1.0);
    knee = max(knee / max(highlightExp, 0.25), 1e-3);
    knee = knee * (1.0 + max(highlightRangeVal, 0.0));
    float shoulderExp = 3.0 / max(highlightExp, 0.25) / max(highlightLiftVal, 0.25);
    float ePow = pow(excess, shoulderExp);
    float kPow = pow(knee, shoulderExp);
    float s = ePow / (ePow + kPow);
    return paperWhiteNits + headroom * s;
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

// Special K / UE-inspired gamut expansion for highlight saturation recovery.
// Expects linear Rec709-relative RGB (scRGB-style). Applied after luminance expansion.
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
    vec3 ChromaAP1 = ColorAP1 / LumaAP1;

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
    vec4 tex = texture(sampler, texcoord0);

    tex = encodingToNits(tex, sourceNamedTransferFunction, sourceTransferFunctionParams.x, sourceTransferFunctionParams.y);
    tex.rgb = (colorimetryTransform * vec4(tex.rgb, 1.0)).rgb;

    float ref = max(destinationReferenceLuminance, 1.0);
    float displayPeak = max(maxDestinationLuminance, ref);
    float peakNits = min(max(maxNits, ref), displayPeak);

    // Align decoded source white to the KDE HDR calibration reference highlight.
    float sourceWhite = max(sourceTransferFunctionParams.x + sourceTransferFunctionParams.y, 1.0);
    tex.rgb *= ref / sourceWhite;

    float alpha = max(tex.a, 0.001);
    vec3 rgb = tex.rgb / alpha;

    float lumaNits = max(dot(rgb, LUMA), 1e-6);
    float t = lumaNits / ref;
    t = max(t - blackPoint, 0.0) / max(1.0 - blackPoint, 1e-6);

    float paperWhiteNits = clamp(midPoint, 80.0, 480.0);
    float Yn = mapTone(t, paperWhiteNits, peakNits, highlightExpansion, highlightLift, highlightRange);
    rgb = rgb * (Yn / lumaNits);

    vec3 sceneMapped = applyColorControls(rgb / ref, 1.0, colorVibrance);
    rgb = sceneMapped * ref;

    if (gamutExpansion > 0.0) {
        rgb = expandGamut(rgb / ref, gamutExpansion) * ref;
    }

    float outLuma = dot(rgb, LUMA);
    if (highlightRange > 0.0 && outLuma > peakNits) {
        float headroom = max(peakNits - paperWhiteNits, 1e-3);
        float over = outLuma - peakNits;
        float roll = max(headroom * highlightRange, 1e-3);
        float compressed = roll * over / (over + roll);
        outLuma = peakNits + compressed;
        rgb *= outLuma / dot(rgb, LUMA);
    }
    outLuma = dot(rgb, LUMA);
    if (outLuma > displayPeak) {
        rgb *= displayPeak / outLuma;
    }
    tex.rgb = max(rgb * alpha, vec3(0.0));
    tex *= modulation;
    fragColor = nitsToDestinationEncoding(tex);
}
