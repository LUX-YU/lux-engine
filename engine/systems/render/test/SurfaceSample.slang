[[vk::binding(0, 0)]] Texture2D<float4> source_image : register(t0);
[[vk::binding(1, 0)]] SamplerState source_sampler : register(s1);

[shader("fragment")] float4 main() : SV_Target0
{
    return source_image.Sample(source_sampler, float2(0.5));
}
