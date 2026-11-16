[[vk::binding(0, 0)]][format("r32f")] RWTexture2D<float> generated : register(u0, space0);

[shader("compute")][numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    generated.GetDimensions(width, height);
    if (all(id.xy < uint2(width, height)))
    {
        generated[id.xy] = all(id.xy == uint2(width, height) - 1) ? 0.9 : 0.2;
    }
}
