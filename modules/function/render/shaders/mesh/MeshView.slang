#include "../Common.slang"

struct View
{
    mat4 world_to_clip;
    vec4 planes[6];
    uvec4 flags;
};

[[vk::binding(0, 0)]] RWStructuredBuffer<View> views : register(u0, space0);

struct Parameters
{
    mat4 view;
    mat4 projection;
};

[[vk::push_constant]] ConstantBuffer<Parameters> parameters : register(b0, space255);

void luxMain()
{
    mat4 clip = mul(parameters.projection, parameters.view);
    mat4 rows = clip;
    views[0].world_to_clip = clip;
    // Homogeneous clip inequalities, forward depth [0, w]. Normalization is unnecessary for
    // the AABB support test; it would also divide by zero for an infinite-far projection.
    views[0].planes[0] = rows[3] + rows[0];
    views[0].planes[1] = rows[3] - rows[0];
    views[0].planes[2] = rows[3] + rows[1];
    views[0].planes[3] = rows[3] - rows[1];
    views[0].planes[4] = rows[2];
    views[0].planes[5] = rows[3] - rows[2];
    views[0].flags = uvec4(1, 0, 0, 0);
}

[shader("compute")][numthreads(1, 1, 1)] void
main(uint3 dispatch : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint lane : SV_GroupIndex)
{
    gl_GlobalInvocationID = dispatch;
    gl_WorkGroupID = groupID;
    gl_LocalInvocationIndex = lane;
    luxMain();
}
