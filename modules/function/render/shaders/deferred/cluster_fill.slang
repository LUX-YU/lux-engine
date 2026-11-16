#include "../Common.slang"
#include "ClusterIntersection.slang"

struct Lux_offsets
{
    uint value;
};

[[vk::binding(1, 0)]] StructuredBuffer<Lux_offsets> offsets : register(t1, space0);

struct Status
{
    uvec4 values;
};

[[vk::binding(2, 0)]] StructuredBuffer<Status> status : register(t2, space0);

struct Lux_indices
{
    uint value;
};

[[vk::binding(3, 0)]] RWStructuredBuffer<Lux_indices> indices : register(u3, space0);

void luxMain()
{
    uint cell = gl_GlobalInvocationID.x;
    if (cell + 1 >= luxLength(offsets) || status[0].values.y != 0)
    {
        return;
    }
    // One writer per cell replaces Legacy's atomic write-head array. Preserve full-light order
    // (points then spots) so the same contributions also have the same floating-point_position sum order.
    // Scan has admitted the entire prefix against this buffer's declared capacity. Count/Fill
    // share both inputs and predicate; each writer's exclusive interval is therefore in bounds.
    uint destination = offsets[cell].value;
    for (uint i = 0; i < light_counts[0].values.y; ++i)
    {
        if (overlapsCluster(bounds[cell], point_lights[i].position_range))
        {
            indices[destination++].value = i;
        }
    }
    for (uint i = 0; i < light_counts[0].values.z; ++i)
    {
        if (overlapsCluster(bounds[cell], spot_lights[i].position_range))
        {
            indices[destination++].value = i | 0x80000000u;
        }
    }
}

[shader("compute")][numthreads(64, 1, 1)] void
main(uint3 dispatch : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint lane : SV_GroupIndex)
{
    gl_GlobalInvocationID = dispatch;
    gl_WorkGroupID = groupID;
    gl_LocalInvocationIndex = lane;
    luxMain();
}
