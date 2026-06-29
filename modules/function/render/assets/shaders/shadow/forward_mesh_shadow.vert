#version 450
#extension GL_ARB_shader_viewport_layer_array : enable
// =========================================================================
//  forward_mesh_shadow.vert — GPU-driven shadow vertex shader (V2)
//
//  Three-stream: reads Transform SSBO (set 1 binding 0).
//  gl_InstanceIndex carries packed (slice_index << 20) | instance_index,
//  written by mesh_cull_unified.comp in shadow mode.
// =========================================================================

// ========== Vertex Input ==========
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inTangent;
layout(location = 3) in vec2 inUV;
layout(location = 4) in vec3 inBitangent;

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
};
layout(set = 0, binding = 0, std430) readonly buffer ShadowSliceBuf {
    ShadowSliceGPU slices[];
} uShadowSlices;

// ========== SET 1: Three-stream instance data ==========
#include "instance.glsl"

layout(set = 1, binding = 0, std430) readonly buffer TransformBuf {
    InstanceTransform transforms[];
} uTransforms;

layout(set = 2, binding = 0, std430) readonly buffer VisibleInstanceBuf {
    uint ids[];
} uVisibleInstances;

// ========== Output ==========
layout(location = 0) out vec2 vUV;

void main()
{
    const uint kSlotBits = 20u;
    const uint kSlotMask = (1u << kSlotBits) - 1u;

    uint visibleIndex = uint(gl_InstanceIndex);
    uint packed    = uVisibleInstances.ids[visibleIndex];
    uint instSlot  = packed & kSlotMask;
    uint sliceIdx  = packed >> kSlotBits;

    mat4 model    = uTransforms.transforms[instSlot].world;
    ShadowSliceGPU s = uShadowSlices.slices[sliceIdx];
    vec4 worldPos = model * vec4(inPosition, 1.0);
    vec4 lightClip = s.light_vp * worldPos;

    // Clip to the original light frustum in X/Y.
    // The atlas remap widens the hardware clip region beyond the tile boundary,
    // so without these clip planes, large geometry (e.g. floor) can bleed into
    // neighbouring tiles that share the same bias-group union scissor.
    gl_ClipDistance[0] = lightClip.w + lightClip.x; // x >= -w
    gl_ClipDistance[1] = lightClip.w - lightClip.x; // x <=  w
    gl_ClipDistance[2] = lightClip.w + lightClip.y; // y >= -w
    gl_ClipDistance[3] = lightClip.w - lightClip.y; // y <=  w

    // Remap XY from light-clip space to atlas tile position.
    // With a single full-atlas viewport the vertex shader positions
    // each triangle in the correct tile, eliminating per-slice
    // vkCmdSetViewport calls on the CPU.
    gl_Position.x = lightClip.x * s.atlas_uv_scale.x
                  + (s.atlas_uv_scale.x + 2.0 * s.atlas_uv_bias.x - 1.0) * lightClip.w;
    gl_Position.y = lightClip.y * s.atlas_uv_scale.y
                  + (s.atlas_uv_scale.y + 2.0 * s.atlas_uv_bias.y - 1.0) * lightClip.w;
    gl_Position.z = lightClip.z;
    gl_Position.w = lightClip.w;
    gl_Layer      = int(s.atlas_layer);

    vUV = inUV;
}
