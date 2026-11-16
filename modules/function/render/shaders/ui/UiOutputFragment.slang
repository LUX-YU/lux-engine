#include "../Common.slang"
[[vk::binding(0, 0)]] Texture2D<float4> linear_color : register(t0, space0);
static vec4 result;

void luxMain()
{
    result = texelFetch(linear_color, ivec2(gl_FragCoord.xy), 0);
#ifdef OUTPUT_SRGB
    vec3 color = max(result.rgb, vec3(0));
    result.rgb = mix(1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055, 12.92 * color, lessThanEqual(color, vec3(0.0031308)));
#endif
}

struct LuxInput
{
    float4 pixel : SV_Position;
};

struct LuxOutput
{
    vec4 result : SV_Target0;
};
[shader("fragment")] LuxOutput main(LuxInput input)
{
    gl_FragCoord = input.pixel;
    luxMain();
    LuxOutput output;
    output.result = result;
    return output;
}
