struct Value
{
    uint value;
};

[[vk::binding(0, 0)]] Texture2D<float4> reference_image : register(t0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> candidate_image : register(t1, space0);
[[vk::binding(2, 0)]] RWStructuredBuffer<Value> differences : register(u2, space0);

[shader("compute")][numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    reference_image.GetDimensions(width, height);
    if (any(id.xy >= uint2(width, height)))
    {
        return;
    }
    float4 a = reference_image.Load(int3(id.xy, 0));
    float4 b = candidate_image.Load(int3(id.xy, 0));
    differences[id.y * width + id.x].value = all(a == b) ? 1u : 2u;
}
