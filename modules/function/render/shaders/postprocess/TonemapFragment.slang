#include "../Common.slang"

[[vk::binding(0, 0)]] Texture2D<float4> hdr_color : register(t0, space0);

struct Parameters
{
    float exposure;
};

[[vk::push_constant]] ConstantBuffer<Parameters> parameters : register(b0, space255);
static vec4 color;

// Legacy tonemap.frag.lglsl::toneMapACES, preserving its actual coefficients.
vec3 toneMapACES(vec3 value)
{
    return clamp((value * (2.51 * value + 0.03)) / (value * (2.43 * value + 0.59) + 0.14), 0.0, 1.0);
}

void luxMain()
{
    vec3 value = max(texelFetch(hdr_color, ivec2(gl_FragCoord.xy), 0).rgb, vec3(0.0)) * exp2(parameters.exposure);
    value = toneMapACES(value);
#ifdef OUTPUT_SRGB
    value = mix(1.055 * pow(value, vec3(1.0 / 2.4)) - 0.055, 12.92 * value, lessThanEqual(value, vec3(0.0031308)));
#endif
    color = vec4(value, 1.0);
}

struct LuxInput
{
    float4 pixel : SV_Position;
};

struct LuxOutput
{
    vec4 color : SV_Target0;
};
[shader("fragment")] LuxOutput main(LuxInput input)
{
    gl_FragCoord = input.pixel;
    luxMain();
    LuxOutput output;
    output.color = color;
    return output;
}
