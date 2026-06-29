#version 450
#extension GL_ARB_shader_viewport_layer_array : enable
// =========================================================================
//  mesh_shadow_vp.vert — GPU-driven shadow vertex shader, VERTEX-POOL variant
//
//  Identical to mesh_shadow.vert except the vertex position comes from
//  the bindless vertex pool array (set 7) instead of a vertex attribute
//  input. Used for all meshes (static and skinned).
// =========================================================================

// ========== SET 0: Shadow Slice VP Matrices ==========
struct ShadowSliceGPU {
    mat4  light_vp;
    float bias;
    float slope_bias;
    float normal_bias;
    float texel_size;
    vec2  atlas_uv_scale;
    vec2  atlas_uv_bias;
    vec2  atlas_inner_uv_min;
    vec2  atlas_inner_uv_max;
    uint  atlas_layer;
    float shadow_near;          // EVSM linearization: light proj near plane
    float shadow_far;           // EVSM linearization: light proj far plane
    uint  depth_is_perspective; // 1 = perspective slice (linearize), 0 = ortho
};
layout(set = 0, binding = 0, std430) readonly buffer ShadowSliceBuf {
    ShadowSliceGPU slices[];
} uShadowSlices;

// ========== SET 1: Three-stream instance data ==========
#include "instance.glsl"

layout(set = 1, binding = 0, std430) readonly buffer TransformBuf {
    InstanceTransform transforms[];
} uTransforms;

layout(set = 1, binding = 1, std430) readonly buffer PropertyBuf {
    InstanceProperty properties[];
} uProperties;

layout(set = 2, binding = 0, std430) readonly buffer VisibleInstanceBuf {
    uint ids[];
} uVisibleInstances;

// ========== SET 3: Bindless vertex pools ==========
// Shadow binds the pool at set 3 (its layout is only 4 sets: slices, instance,
// visible, pool) — avoids 4 placeholder sets a set-7 layout would need.
#define LUX_VERTEX_POOL_SET 3
#include "vertex_pool.glsl"

// ========== Output ==========
layout(location = 0) out vec2 vUV;
// EVSM caster reads these to warp a LINEAR depth (perspective slices). The PCF
// depth-only caster ignores them — extra vertex outputs are valid in Vulkan.
layout(location = 1) flat out float vShadowNear;
layout(location = 2) flat out float vShadowFar;
layout(location = 3) flat out uint  vDepthPersp;

void main()
{
    const uint kSlotBits = 20u;
    const uint kSlotMask = (1u << kSlotBits) - 1u;

    uint visibleIndex = uint(gl_InstanceIndex);
    uint packed    = uVisibleInstances.ids[visibleIndex];
    uint instSlot  = packed & kSlotMask;
    uint sliceIdx  = packed >> kSlotBits;

    InstanceProperty prop = uProperties.properties[instSlot];
    // gl_VertexIndex == draw vertexOffset + index == input_vertex_offset + local
    // Subtract the bias so vertex_base is the honest output base.
    uint localVertex = uint(gl_VertexIndex) - prop.input_vertex_offset;
    MeshVertex mv = luxFetchMeshVertex(prop.vertex_pool_id,
                                       prop.vertex_base,
                                       localVertex);

    mat4 model    = uTransforms.transforms[instSlot].world;
    ShadowSliceGPU s = uShadowSlices.slices[sliceIdx];
    vec4 worldPos = model * vec4(mv.position, 1.0);
    vec4 lightClip = s.light_vp * worldPos;

    // Clip to the original light frustum in X/Y (see mesh_shadow.vert).
    gl_ClipDistance[0] = lightClip.w + lightClip.x; // x >= -w
    gl_ClipDistance[1] = lightClip.w - lightClip.x; // x <=  w
    gl_ClipDistance[2] = lightClip.w + lightClip.y; // y >= -w
    gl_ClipDistance[3] = lightClip.w - lightClip.y; // y <=  w

    gl_Position.x = lightClip.x * s.atlas_uv_scale.x
                  + (s.atlas_uv_scale.x + 2.0 * s.atlas_uv_bias.x - 1.0) * lightClip.w;
    gl_Position.y = lightClip.y * s.atlas_uv_scale.y
                  + (s.atlas_uv_scale.y + 2.0 * s.atlas_uv_bias.y - 1.0) * lightClip.w;
    gl_Position.z = lightClip.z;
    gl_Position.w = lightClip.w;
    gl_Layer      = int(s.atlas_layer);

    vUV = mv.uv;
    vShadowNear = s.shadow_near;
    vShadowFar  = s.shadow_far;
    vDepthPersp = s.depth_is_perspective;
}
