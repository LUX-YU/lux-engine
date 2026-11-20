[[vk::binding(0, 0)]] RWStructuredBuffer<uint> generated : register(u0, space0);

struct Parameters
{
    uint value;
};

[[vk::push_constant]] ConstantBuffer<Parameters> parameters : register(b0, space255);

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    generated[id.x] = parameters.value;
}
