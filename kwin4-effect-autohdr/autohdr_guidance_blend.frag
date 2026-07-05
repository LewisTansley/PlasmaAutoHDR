#version 140

uniform sampler2D sampler;
uniform sampler2D glslSampler;

in vec2 texcoord0;
out vec4 fragColor;

void main()
{
    vec4 ort = texture(sampler, texcoord0);
    vec4 glsl = texture(glslSampler, texcoord0);
    fragColor = vec4(
        max(ort.r, glsl.r),
        max(ort.g, glsl.g),
        max(ort.b, glsl.b),
        max(ort.a, glsl.a));
}
