[[vk::binding(0, 0)]] Texture2D<float4> uTex[] : register(t0, space0);
[[vk::binding(0, 1)]] SamplerState uTexSamplers[] : register(s0, space1);

struct Parameters
{
    uint image_index;
    uint sampler_index;
};

[[vk::push_constant]] ConstantBuffer<Parameters> parameters : register(b0, space255);

[shader("fragment")]
float4 main() : SV_Target0
{
    const uint image = NonUniformResourceIndex(parameters.image_index);
    const uint sampler = NonUniformResourceIndex(parameters.sampler_index);
    return uTex[image].SampleLevel(uTexSamplers[sampler], float2(0.5, 0.5), 0);
}
