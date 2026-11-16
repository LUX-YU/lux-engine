#include "../Common.slang"

// Records/chunk indices address the dense live-group table, not recycled CPU logical slots.
struct DrawGroup
{
    uint index_count;
    uint candidate_count;
    uint first_index;
    int vertex_offset;
    uint candidate_offset;
    uint visible_offset;
    uint visible_stride;
    uint view_count;
    vec4 center;
    vec4 half_extent;
};

[[vk::binding(0, 0)]] StructuredBuffer<DrawGroup> groups : register(t0, space0);

struct Lux_visible
{
    uint value;
};

[[vk::binding(1, 0)]] RWStructuredBuffer<Lux_visible> visible : register(u1, space0);

void luxMain()
{
    uint stream = gl_GlobalInvocationID.x;
    uint streams_per_group = groups[0].view_count * 2;
    uint group_index = stream / streams_per_group;
    if (group_index >= luxLength(groups))
    {
        return;
    }
    DrawGroup group = groups[group_index];
    if (group.candidate_count == 0)
    {
        return;
    }
    uint view_winding = stream % streams_per_group;
    visible[group.visible_offset + view_winding * group.visible_stride + 1].value = 0;
}

[shader("compute")][numthreads(64, 1, 1)] void
main(uint3 dispatch : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint lane : SV_GroupIndex)
{
    gl_GlobalInvocationID = dispatch;
    gl_WorkGroupID = groupID;
    gl_LocalInvocationIndex = lane;
    luxMain();
}
