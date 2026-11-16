#include "../Common.slang"

// Legacy's 128-lane contiguous-chunk Blelloch scan; saturated addition prevents wrapping
// before the capacity check. The sentinel is written even for an empty light set.

struct Lux_counts
{
    uint value;
};

[[vk::binding(0, 0)]] StructuredBuffer<Lux_counts> counts : register(t0, space0);

struct Lux_offsets
{
    uint value;
};

[[vk::binding(1, 0)]] RWStructuredBuffer<Lux_offsets> offsets : register(u1, space0);

struct Status
{
    uvec4 values;
};

[[vk::binding(2, 0)]] RWStructuredBuffer<Status> status : register(u2, space0);

struct Parameters
{
    uint cluster_count;
    uint capacity;
};

[[vk::push_constant]] ConstantBuffer<Parameters> parameters : register(b0, space255);
groupshared uint sums[128];
groupshared uint total;

uint add(uint a, uint b)
{
    return a > 0xffffffffu - b ? 0xffffffffu : a + b;
}

void luxMain()
{
    uint lane = gl_LocalInvocationIndex;
    uint chunk = parameters.cluster_count / 128 + uint(parameters.cluster_count % 128 != 0);
    uint begin = min(lane * chunk, parameters.cluster_count);
    uint end = min(begin + min(chunk, parameters.cluster_count - begin), parameters.cluster_count);
    uint local = 0;
    for (uint i = begin; i < end; ++i)
    {
        local = add(local, counts[i].value);
    }
    sums[lane] = local;
    barrier();
    for (uint stride = 1; stride < 128; stride <<= 1)
    {
        uint i = (lane + 1) * stride * 2 - 1;
        if (i < 128)
        {
            sums[i] = add(sums[i], sums[i - stride]);
        }
        barrier();
    }
    if (lane == 0)
    {
        total = sums[127];
        sums[127] = 0;
    }
    barrier();
    for (uint stride = 64; stride != 0; stride >>= 1)
    {
        uint i = (lane + 1) * stride * 2 - 1;
        if (i < 128)
        {
            uint before = sums[i - stride];
            sums[i - stride] = sums[i];
            sums[i] = add(sums[i], before);
        }
        barrier();
    }
    uint running = sums[lane];
    for (uint i = begin; i < end; ++i)
    {
        offsets[i].value = running;
        running = add(running, counts[i].value);
    }
    if (lane == 0)
    {
        offsets[parameters.cluster_count].value = total;
        status[0].values.x = total;
        status[0].values.y |= (total > parameters.capacity ? 1u : 0u) | (total == 0xffffffffu ? 4u : 0u);
    }
}

[shader("compute")][numthreads(128, 1, 1)] void
main(uint3 dispatch : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint lane : SV_GroupIndex)
{
    gl_GlobalInvocationID = dispatch;
    gl_WorkGroupID = groupID;
    gl_LocalInvocationIndex = lane;
    luxMain();
}
