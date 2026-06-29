#version 450
#extension GL_ARB_shader_viewport_layer_array : enable
// =========================================================================
//  shadow_depth_vp.vert — depth-only shadow caster vertex shader, VERTEX-POOL
//  variant. Pairs with shadow_depth.frag (vUV only).
//
//  Mirrors mesh_shadow_vp.vert (same descriptor sets, same instance / visible
//  / vertex-pool path) but drops the EVSM-only varyings (vShadowNear /
//  vShadowFar / vDepthPersp). Those three are read by shadow_evsm_caster.frag
//  to linearize / warp perspective depth; the PCF / depth-only caster does
//  not read them. Keeping a separate, thinner vert here means the SPIR-V
//  interface validator is happy when this vert is paired with shadow_depth.frag
//  (no OutputNotConsumed warnings at locations 1/2/3).
//
//  EVSM caster pipelines use mesh_shadow_vp.vert (selected via
//  EVSMShadowTechnique::casterVertVariant in MeshShadowFeature::ensureCasterPipeline)
//  so they have the warp inputs.
// =========================================================================

// ========== SET 0: Shadow Slice VP Matrices ==========
// NOTE: the struct layout MUST stay byte-for-byte identical to the one in
// mesh_shadow_vp.vert / shadow_evsm_caster.frag — the same buffer is bound
// at set 0 for both pipelines. We simply don't read the EVSM-only fields here.
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
    float shadow_near;
    float shadow_far;
    uint  depth_is_perspective;
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
#define LUX_VERTEX_POOL_SET 3
#include "vertex_pool.glsl"

// ========== Output (depth-only — vUV only) ==========
layout(location = 0) out vec2 vUV;

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

    // Clip to the original light frustum in X/Y (see mesh_shadow_vp.vert).
    gl_ClipDistance[0] = lightClip.w + lightClip.x; // x >= -w
    gl_ClipDistance[1] = lightClip.w - lightClip.x; // x <=  w
    gl_ClipDistance[2] = lightClip.w + lightClip.y; // y >= -w
    gl_ClipDistance[3] = lightClip.w - lightClip.y; // y <=  w

    // Remap XY from light-clip space to atlas tile position. Single
    // full-atlas viewport ⇒ vertex shader places each triangle into its
    // tile, eliminating per-slice vkCmdSetViewport calls.
    gl_Position.x = lightClip.x * s.atlas_uv_scale.x
                  + (s.atlas_uv_scale.x + 2.0 * s.atlas_uv_bias.x - 1.0) * lightClip.w;
    gl_Position.y = lightClip.y * s.atlas_uv_scale.y
                  + (s.atlas_uv_scale.y + 2.0 * s.atlas_uv_bias.y - 1.0) * lightClip.w;
    gl_Position.z = lightClip.z;
    gl_Position.w = lightClip.w;
    gl_Layer      = int(s.atlas_layer);

    vUV = mv.uv;
}
