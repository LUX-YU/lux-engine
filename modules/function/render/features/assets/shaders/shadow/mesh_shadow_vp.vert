#version 450
#extension GL_ARB_shader_viewport_layer_array : enable
#ifdef LUX_INSTANCE_PAGED
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#endif
// =========================================================================
//  mesh_shadow_vp.vert — GPU-driven shadow vertex shader, VERTEX-POOL variant
//
//  Identical to mesh_shadow.vert except the vertex position comes from
//  the bindless vertex pool array (set 7) instead of a vertex attribute
//  input. Used for all meshes (static and skinned).
// =========================================================================

// ========== SET 0: Shadow Slice VP Matrices ==========
#include "shadow_common.glsl"
layout(set = 0, binding = 0, std430) readonly buffer ShadowSliceBuf {
    ShadowSliceGPU slices[];
} uCasterShadowSlices;
#include "shadow_caster_config.glsl"

// ========== SET 1: Three-stream instance data ==========
#include "instance.glsl"

layout(set = 1, binding = 0, std430) readonly buffer TransformBuf {
#ifdef LUX_INSTANCE_PAGED
    uint64_t leaf_addresses[];
#else
    InstanceTransform transforms[];
#endif
} uTransforms;

layout(set = 1, binding = 1, std430) readonly buffer PropertyBuf {
#ifdef LUX_INSTANCE_PAGED
    uint64_t leaf_addresses[];
#else
    InstanceProperty properties[];
#endif
} uProperties;

layout(set = 2, binding = 0, std430) readonly buffer VisibleInstanceBuf {
    GpuVisibleInstance entries[];
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
layout(location = 4) flat out float vTransitionCoverage;
layout(location = 5) flat out uint vTransitionSeed;
layout(location = 6) flat out uint vTransitionFadeOut;

void main()
{
    uint visibleIndex = uint(gl_InstanceIndex);
    GpuVisibleInstance visible =
        uVisibleInstances.entries[visibleIndex];
    uint instSlot = visible.instance_slot;
    uint sliceIdx = visible.slice_index;

#ifdef LUX_INSTANCE_PAGED
    InstanceProperty prop = luxLoadInstanceProperty(
        uProperties.leaf_addresses[luxInstanceRootIndex(instSlot)],
        instSlot);
    InstanceTransform transform = luxLoadInstanceTransform(
        uTransforms.leaf_addresses[luxInstanceRootIndex(instSlot)],
        instSlot);
#else
    InstanceProperty prop = uProperties.properties[instSlot];
    InstanceTransform transform = uTransforms.transforms[instSlot];
#endif
    // gl_VertexIndex == draw vertexOffset + index == input_vertex_offset + local
    // Subtract the bias so vertex_base is the honest output base.
    uint localVertex = uint(gl_VertexIndex) - prop.input_vertex_offset;
    MeshVertex mv = luxFetchMeshVertex(prop.vertex_pool_id,
                                       prop.vertex_base,
                                       localVertex);

    ShadowSliceGPU s = uCasterShadowSlices.slices[sliceIdx];
    mat4 model = luxSpatialModel(
        transform,
        s.origin_page.xyz,
        s.origin_local_page_size.xyz,
        s.origin_local_page_size.w
    );
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
    vTransitionCoverage = luxTransitionCoverage(
        prop, uCasterConfig.scene_time);
    vTransitionSeed = prop.transition_seed;
    vTransitionFadeOut =
        (prop.transition_flags & LUX_TRANSITION_FADE_OUT) != 0u ? 1u : 0u;
}
