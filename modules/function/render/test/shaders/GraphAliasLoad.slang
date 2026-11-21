struct Word
{
    uint value;
};

[[vk::binding(0, 0)]] StructuredBuffer<Word> seed : register(t0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<Word> result : register(u1, space0);

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    uint value = seed[thread.x].value ^ thread.x;
    [loop] for (uint i = 0; i < 4096; ++i)
    {
        value ^= value >> 13;
        value = value * 1664525u + i;
    }
    result[thread.x].value = value;
}
