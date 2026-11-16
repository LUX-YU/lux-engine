[[vk::binding(0, 5)]] Texture2D<float4> colors[2] : register(t0, space5);
[[vk::binding(2, 5)]] Texture2D<float4> depth_image : register(t2, space5);
[[vk::binding(3, 5)]] SamplerState source_sampler : register(s3, space5);

[shader("fragment")] float4 main() : SV_Target0
{
    float red = colors[0].Sample(source_sampler, float2(0.5)).r;
    float green = colors[1].Sample(source_sampler, float2(0.5)).g;
    float depth = depth_image.Sample(source_sampler, float2(0.5)).r;
    return float4(red, green, depth, 1);
}
