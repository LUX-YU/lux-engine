struct Parameters
{
    uint value;
};

[[vk::binding(4, 2)]] ConstantBuffer<Parameters> tint : register(b4, space2);
[[vk::binding(0, 2)]] Texture2DArray<float4> tiles[2] : register(t0, space2);
[[vk::binding(2, 2)]] SamplerState sampler_state : register(s2, space2);

[shader("fragment")]
float4 main() : SV_Target0
{
    const float3 uv = float3(0.5, 0.5, 0);
    const float4 first = tiles[0].SampleLevel(sampler_state, uv, 0);
    const float4 second = tiles[1].SampleLevel(sampler_state, uv, 0);
    return float4(first.r, (tint.value & 255u) / 255.0, second.b, 1);
}
