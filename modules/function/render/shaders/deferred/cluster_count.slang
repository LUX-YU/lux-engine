#include "../Common.slang"
#include "ClusterIntersection.slang"

struct Lux_counts
{
    uint value;
};

[[vk::binding(1, 0)]] RWStructuredBuffer<Lux_counts> counts : register(u1, space0);

void luxMain()
{
    uint cell = gl_GlobalInvocationID.x;
    if (cell >= luxLength(counts))
    {
        return;
    }
    uint count = 0;
    for (uint i = 0; i < light_counts[0].values.y; ++i)
    {
        if (overlapsCluster(bounds[cell], point_lights[i].position_range))
        {
            count = saturatedAdd(count, 1u);
        }
    }
    for (uint i = 0; i < light_counts[0].values.z; ++i)
    {
        if (overlapsCluster(bounds[cell], spot_lights[i].position_range))
        {
            count = saturatedAdd(count, 1u);
        }
    }
    counts[cell].value = count;
}

[shader("compute")][numthreads(64, 1, 1)] void
main(uint3 dispatch : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint lane : SV_GroupIndex)
{
    gl_GlobalInvocationID = dispatch;
    gl_WorkGroupID = groupID;
    gl_LocalInvocationIndex = lane;
    luxMain();
}
