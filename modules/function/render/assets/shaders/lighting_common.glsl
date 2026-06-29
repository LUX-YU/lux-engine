#ifndef LIGHTING_COMMON_GLSL
#define LIGHTING_COMMON_GLSL
// =========================================================================
//  lighting_common.glsl — shared declarations for all forward fragment shaders
//
//  Provides: Scene UBO (set 0), bindless textures (set 2), light SSBOs (set 3),
//            FS input varyings, lighting functions, and helper utilities.
//
//  Usage: #include "lighting_common.glsl" at the top of each per-material frag.
//  Requires: #version 450 and GL_EXT_nonuniform_qualifier declared BEFORE this include.
// =========================================================================

// ========== SET 0: View (std140) ==========
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

// ====== Shared Push Constants (scene/view indices, offset 0) ======
layout(push_constant) uniform SharedPC {
    uint scene_index;
    uint view_index;
} uPC;

// ========== FS Input ==========
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vWorldNormal;
layout(location = 2) in vec3 vViewDir;
layout(location = 3) in vec2 vUV;
layout(location = 4) in flat uint vMatIndex;
layout(location = 5) in vec3 vWorldTangent;
layout(location = 6) in vec3 vWorldBitangent;

// ========== Output ==========
layout(location = 0) out vec4 outColor;

// ========== SET 2: Bindless texture array (combined image sampler) ==========
layout(set = 2, binding = 0) uniform sampler2D uTex[];

// ========== SET 3: Light SSBOs (std430) ==========
#include "light_types.glsl"

layout(set = 3, binding = 0, std430) readonly buffer SpotLightBuf {
    SpotLightGPU lights[];
} uSpotLights;

layout(set = 3, binding = 1, std430) readonly buffer DirectionalLightBuf {
    DirectionalLightGPU lights[];
} uDirectionalLights;

layout(set = 3, binding = 2, std430) readonly buffer PointLightBuf {
    PointLightGPU lights[];
} uPointLights;

layout(set = 3, binding = 3, std430) readonly buffer AreaLightBuf {
    AreaLightGPU lights[];
} uAreaLights;

// ========== Common material helper structs ==========
#include "material_types.glsl"

// ========== Ambient Light Parameters ==========
// SSOT for the ambient term across all lit paths. Must match
// deferred_lighting.frag and the material-graph forward emitter (both 0.03);
// the builtin forward path previously diverged at 0.3 (10x), so the same
// material rendered 10x brighter ambient in forward (thumbnails/preview) than
// in deferred (main view) — breaking the forward/deferred parity the
// material-graph unification guarantees. (medium, ambient-parity)
const float kAmbient = 0.03;

// ========== Utilities ==========
bool hasBit(uint m, uint b) { return (m & (1u << b)) != 0u; }

// Tangent-space normal mapping (shared SSOT; uTex declared above is in scope).
#include "lighting_tbn.glsl"

#endif // LIGHTING_COMMON_GLSL
