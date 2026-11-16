[[vk::binding(0, 0)]] Texture2DArray<float> depths : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState ordinary : register(s1, space0);
[[vk::binding(2, 0)]] SamplerComparisonState comparison : register(s2, space0);

[shader("fragment")] float4 main() : SV_Target0
{
    return float4(
        depths.SampleCmpLevelZero(comparison, float3(0.5, 0.5, 0), 0.5),
        depths.SampleCmpLevelZero(comparison, float3(0.5, 0.5, 1), 0.5),
        depths.Sample(ordinary, float3(0.5, 0.5, 0)),
        1
    );
}
