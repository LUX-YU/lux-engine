[[vk::binding(0, 0)]] Texture2D<float4> first_image : register(t0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> second_image : register(t1, space0);
[[vk::binding(2, 0)]][format("rgba16f")] RWTexture2D<float4> generated : register(u2, space0);

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    generated.GetDimensions(width, height);
    if (all(id.xy < uint2(width, height)))
    {
        generated[id.xy] = (first_image.Load(int3(id.xy, 0)) + second_image.Load(int3(id.xy, 0))) * 0.5;
    }
}
