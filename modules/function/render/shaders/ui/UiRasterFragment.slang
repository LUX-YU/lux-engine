#include "../Common.slang"
[[vk::binding(0, 0)]] Texture2D<float4> source_images[] : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState source_sampler : register(s1, space0);

struct Parameters
{
    vec2 scale;
    vec2 translation;
    uint source_index;
};

[[vk::push_constant]] ConstantBuffer<Parameters> parameters : register(b0, space255);
static vec2 texture_uv;
static vec4 color;
static vec4 result;

void luxMain()
{
    result = color * source_images[NonUniformResourceIndex(parameters.source_index)].Sample(source_sampler, texture_uv);
}

struct LuxInput
{
    [[vk::location(0)]] vec2 texture_uv : TEXCOORD0;
    [[vk::location(1)]] vec4 color : TEXCOORD1;
    float4 pixel : SV_Position;
};

struct LuxOutput
{
    vec4 result : SV_Target0;
};
[shader("fragment")] LuxOutput main(LuxInput input)
{
    texture_uv = input.texture_uv;
    color = input.color;
    gl_FragCoord = input.pixel;
    luxMain();
    LuxOutput output;
    output.result = result;
    return output;
}
