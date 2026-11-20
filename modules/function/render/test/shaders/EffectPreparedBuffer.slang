[[vk::binding(0, 0)]] StructuredBuffer<uint> prepared : register(t0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> generated : register(u1, space0);

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    generated[id.x] = (id.x & 1) == 0 ? prepared[1] : prepared[prepared[0] - 1];
}
