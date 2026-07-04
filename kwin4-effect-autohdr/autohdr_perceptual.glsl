// AutoHDR perceptual luminance/chroma separation (PQ remapping in XYZ)

const float AUTOHDR_PQ_N = 0.1593017578125;
const float AUTOHDR_PQ_RCP_N = 6.2773946361256;
const float AUTOHDR_PQ_M = 78.84375;
const float AUTOHDR_PQ_RCP_M = 0.0126833133693186;
const float AUTOHDR_PQ_C1 = 0.8359375;
const float AUTOHDR_PQ_C2 = 18.8515625;
const float AUTOHDR_PQ_C3 = 18.6875;

vec3 autohdrXyzToRec709(vec3 xyz)
{
    return vec3(
        dot(xyz, vec3(3.240969896, -1.537383198, -0.498610765)),
        dot(xyz, vec3(-0.969243646, 1.875967503, 0.041555058)),
        dot(xyz, vec3(0.055630080, -0.203976959, 1.056971550))
    );
}

float autohdrLinearToPq(float x, float maxPqValue)
{
    float normalized = pow(max(x, 0.0) / max(maxPqValue, 1e-6), AUTOHDR_PQ_N);
    float nd = (AUTOHDR_PQ_C1 + AUTOHDR_PQ_C2 * normalized) / (1.0 + AUTOHDR_PQ_C3 * normalized);
    return pow(nd, AUTOHDR_PQ_M);
}

vec3 autohdrLinearToPq(vec3 x, float maxPqValue)
{
    vec3 normalized = pow(max(x, vec3(0.0)) / max(maxPqValue, 1e-6), vec3(AUTOHDR_PQ_N));
    vec3 nd = (AUTOHDR_PQ_C1 + AUTOHDR_PQ_C2 * normalized) / (1.0 + AUTOHDR_PQ_C3 * normalized);
    return pow(nd, vec3(AUTOHDR_PQ_M));
}

vec4 autohdrLinearToPq(vec4 x, float maxPqValue)
{
    vec4 normalized = pow(max(x, vec4(0.0)) / maxPqValue, vec4(AUTOHDR_PQ_N));
    vec4 nd = (AUTOHDR_PQ_C1 + AUTOHDR_PQ_C2 * normalized) / (1.0 + AUTOHDR_PQ_C3 * normalized);
    return pow(nd, vec4(AUTOHDR_PQ_M));
}

float autohdrPqToLinear(float x, float maxPqValue)
{
    float pq = pow(max(x, 0.0), AUTOHDR_PQ_RCP_M);
    float nd = max(pq - AUTOHDR_PQ_C1, 0.0) / (AUTOHDR_PQ_C2 - AUTOHDR_PQ_C3 * pq);
    return pow(nd, AUTOHDR_PQ_RCP_N) * maxPqValue;
}

vec3 autohdrPqToLinear(vec3 x, float maxPqValue)
{
    vec3 pq = pow(max(x, vec3(0.0)), vec3(AUTOHDR_PQ_RCP_M));
    vec3 nd = max(pq - AUTOHDR_PQ_C1, vec3(0.0)) / (AUTOHDR_PQ_C2 - AUTOHDR_PQ_C3 * pq);
    return pow(nd, vec3(AUTOHDR_PQ_RCP_N)) * maxPqValue;
}

vec4 autohdrPqToLinear(vec4 x, float maxPqValue)
{
    vec4 pq = pow(max(x, vec4(0.0)), vec4(AUTOHDR_PQ_RCP_M));
    vec4 nd = max(pq - AUTOHDR_PQ_C1, vec4(0.0)) / (AUTOHDR_PQ_C2 - AUTOHDR_PQ_C3 * pq);
    return pow(nd, vec4(AUTOHDR_PQ_RCP_N)) * maxPqValue;
}

float autohdrComputePqMul(float yIn, float yOut, vec4 pqParams)
{
    float p0 = pqParams.x;
    float p1 = pqParams.y;
    float p3 = pqParams.z;

    float pqIn = autohdrLinearToPq(yIn, p0);
    float pqOut = autohdrLinearToPq(yOut * p3, p1);
    return pqOut / max(pqIn, 1e-6);
}

// PQ perceptual remap in XYZ; colorIntensity blends Y-only vs full-XYZ PQ boost.
// localColorIntensity >= 0 selects per-pixel blend; negative uses global colorIntensity only.
vec3 applyPerceptualLuminanceMap(vec3 rgbNits, float yIn, float yOut, float colorIntensity, vec4 pqParams,
                                 float localColorIntensity)
{
    float effectiveIntensity = colorIntensity;
    if (localColorIntensity >= 0.0) {
        effectiveIntensity = localColorIntensity;
    }
    float p0 = pqParams.x;
    float p1 = pqParams.y;
    float p3 = pqParams.z;

    if (p0 <= 0.1) {
        return rgbNits;
    }

    float scale = yOut / max(yIn, 1e-6);
    if (abs(scale - 1.0) < 1e-4) {
        return rgbNits;
    }

    vec3 xyz = autohdrRec709ToXYZ(rgbNits);
    float fLuma = max(xyz.y, 0.0);
    if (fLuma <= 0.0) {
        return rgbNits;
    }

    float pqMul = autohdrComputePqMul(yIn, yOut, pqParams);
    vec3 xyzResult;

    if (effectiveIntensity <= 0.001) {
        float newLuma =
            autohdrPqToLinear(autohdrLinearToPq(fLuma, p0) * pqMul, p1) / p3;
        xyzResult = xyz * (newLuma / fLuma);
    } else if (effectiveIntensity >= 0.999) {
        xyzResult = autohdrPqToLinear(autohdrLinearToPq(xyz, p0) * pqMul, p1) / p3;
    } else {
        vec4 newColor =
            autohdrPqToLinear(autohdrLinearToPq(vec4(xyz, fLuma), p0) * pqMul, p1) / p3;
        vec3 lumaOnlyXyz = xyz * (newColor.w / fLuma);
        xyzResult = mix(lumaOnlyXyz, newColor.rgb, effectiveIntensity);
    }

    return max(autohdrXyzToRec709(xyzResult), vec3(0.0));
}
