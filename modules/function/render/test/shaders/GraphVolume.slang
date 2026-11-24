struct Word
{
    uint value;
};

[[vk::binding(0, 0)]] StructuredBuffer<Word> planes : register(t0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<Word> result : register(u1, space0);

[shader("compute")]
[numthreads(2, 1, 2)]
void main(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x == 0 && thread.z < 4)
        result[thread.z].value = planes[(thread.z & 1) * 128].value + thread.z * 16;
}
