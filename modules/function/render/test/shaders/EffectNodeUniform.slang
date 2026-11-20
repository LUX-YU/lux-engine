[[vk::binding(0, 0)]] RWStructuredBuffer<uint> generated : register(u0, space0);

struct Parameters
{
    uint4 words;
};

[[vk::binding(1, 0)]] ConstantBuffer<Parameters> uniform_block : register(b1, space0);

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    generated[id.x] = uniform_block.words[id.x % 4];
}
