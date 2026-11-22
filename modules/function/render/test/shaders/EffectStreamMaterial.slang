#ifndef RECORD_TABLE
#define RECORD_TABLE uMats
#endif

struct TextureReference
{
    uint representation_index;
    uint resource_index;
    uint sampler_index;
    uint flags;
};

struct MaterialRecord
{
    float4 params[1];
    TextureReference tex[2];
};

[[vk::binding(0, 0)]] Texture2D<float4> uTex[] : register(t0, space0);
[[vk::binding(0, 1)]] SamplerState uTexSamplers[] : register(s0, space1);
[[vk::binding(0, 2)]] StructuredBuffer<MaterialRecord> RECORD_TABLE[] : register(t0, space2);

struct Parameters
{
    uint buffer_index;
    uint record_index;
};

[[vk::push_constant]] ConstantBuffer<Parameters> parameters : register(b0, space255);

[shader("fragment")]
float4 main() : SV_Target0
{
    const MaterialRecord value = RECORD_TABLE[NonUniformResourceIndex(parameters.buffer_index)][parameters.record_index];
    const TextureReference texture = value.tex[0];
    if ((texture.flags & 1u) != 0)
        return value.params[0];
    return value.params[0] * uTex[NonUniformResourceIndex(texture.resource_index)].SampleLevel(
        uTexSamplers[NonUniformResourceIndex(texture.sampler_index)], float2(0.5, 0.5), 0
    );
}
