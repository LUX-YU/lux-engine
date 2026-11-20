[[vk::binding(0, 0)]][format("rgba16f")] RWTexture2D<float4> generated : register(u0, space0);

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    generated.GetDimensions(width, height);
    if (all(id.xy < uint2(width, height)))
    {
        generated[id.xy] = float4(0.25, 0.5, 0.75, 1.0);
    }
}
