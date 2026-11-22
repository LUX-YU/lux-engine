[[vk::binding(0, 0)]] Texture2D<float4> selected : register(t0, space0);
[[vk::binding(1, 0)]][format("rgba8")] RWTexture2D<float4> result : register(u1, space0);

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    result[id.xy] = selected.Load(int3(id.xy, 0));
}
