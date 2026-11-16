#include "../Common.slang"

struct Parameters
{
    uint shared_members;
};

[[vk::push_constant]] ConstantBuffer<Parameters> parameters : register(b0, space255);

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

[[vk::binding(1, 0)]] StructuredBuffer<Lux_visible> visible : register(t1, space0);

struct Lux_commands
{
    uint value;
};

[[vk::binding(2, 0)]] RWStructuredBuffer<Lux_commands> commands : register(u2, space0);

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
    uint view_winding = stream % streams_per_group;
    uint instances =
        group.candidate_count == 0 ? 0 : visible[group.visible_offset + view_winding * group.visible_stride + 1].value;
    uint offset = stream * 5;
    commands[offset].value = group.index_count;
    commands[offset + 1].value = instances;
    commands[offset + 2].value = group.first_index;
    commands[offset + 3].value = uint(group.vertex_offset);
    commands[offset + 4].value =
        parameters.shared_members != 0 ? group.visible_offset + view_winding * group.visible_stride : 0;
}

[shader("compute")][numthreads(64, 1, 1)] void
main(uint3 dispatch : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint lane : SV_GroupIndex)
{
    gl_GlobalInvocationID = dispatch;
    gl_WorkGroupID = groupID;
    gl_LocalInvocationIndex = lane;
    luxMain();
}
