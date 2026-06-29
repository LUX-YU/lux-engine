#version 450
// =========================================================================
//  highlight_mask.vert — highlight-outline MASK vertex shader (vertex-pool).
//
//  Draws the silhouette of highlighted (Step 1: ALL visible) instances. Mirrors
//  forward_mesh_vp.vert (same set-0 ViewBuffer camera, set-1 instance, set-5
//  visible, set-7 bindless vertex pool — so it draws BOTH static and SKINNED
//  meshes in their live pose) but outputs ONLY gl_Position: the mask fragment
//  shader reads no varyings, so a thinner interface keeps the SPIR-V validator
//  free of OutputNotConsumed warnings (same rationale as shadow_depth_vp.vert).
// =========================================================================

// ========== SET 0: View Buffer (camera VP) ==========
struct ViewGpuData {
    mat4 view;
    mat4 proj;
    mat4 inv_view;
    mat4 inv_proj;
    vec4 cam_pos;
    vec4 viewport;
};
layout(set = 0, binding = 1, std430) readonly buffer ViewBuffer {
    ViewGpuData views[];
} uViews;

layout(push_constant) uniform SharedPC {
    uint scene_index;
    uint view_index;
} uPC;

// ========== SET 1: Three-stream instance data ==========
#include "instance.glsl"

layout(set = 1, binding = 0, std430) readonly buffer TransformBuf {
    InstanceTransform transforms[];
} uTransforms;

layout(set = 1, binding = 1, std430) readonly buffer PropertyBuf {
    InstanceProperty properties[];
} uProperties;

layout(set = 5, binding = 0, std430) readonly buffer VisibleInstanceBuf {
    uint ids[];
} uVisibleInstances;

// ========== SET 7: Bindless vertex pools ==========
#include "vertex_pool.glsl"

// Per-instance flag word forwarded to the fragment shader, which discards
// non-highlighted fragments. flat = no interpolation (per-instance constant).
layout(location = 0) flat out uint vFlags;

void main()
{
    uint visibleIndex = uint(gl_InstanceIndex);
    uint drawSlot     = uVisibleInstances.ids[visibleIndex];

    InstanceProperty prop = uProperties.properties[drawSlot];
    vFlags = prop.flags;

    // gl_VertexIndex == input_vertex_offset + local; subtract the bias so
    // vertex_base stays the honest output base (mirrors forward_mesh_vp.vert).
    uint localVertex = uint(gl_VertexIndex) - prop.input_vertex_offset;
    MeshVertex mv = luxFetchMeshVertex(prop.vertex_pool_id,
                                       prop.vertex_base,
                                       localVertex);

    mat4 model    = uTransforms.transforms[drawSlot].world;
    vec4 worldPos = model * vec4(mv.position, 1.0);

    gl_Position = uViews.views[uPC.view_index].proj
                * uViews.views[uPC.view_index].view
                * worldPos;
}
