#include "../Common.slang"
#include "../SceneView.slang"

#include "VisibilityView.slang"

[[vk::binding(0, 0)]] RWStructuredBuffer<VisibilityView> views : register(u0, space0);

[[vk::binding(1, 0)]] StructuredBuffer<SceneViewGpu> scene_view : register(t1, space0);

void luxMain()
{
    mat4 clip = mul(scene_view[0].projection, scene_view[0].view);
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
