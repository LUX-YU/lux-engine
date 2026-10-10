#ifndef LIGHT_TYPES_GLSL
#define LIGHT_TYPES_GLSL
// =========================================================================
//  light_types.glsl — GPU light structure definitions
//
//  Shared between forward (set 3) and deferred (set 2) lighting passes.
//  This file contains ONLY struct types — no layout bindings.
//  Include this before declaring per-pass SSBO bindings.
// =========================================================================

struct DirectionalLightGPU {
    vec3  color;     uint _pad0;
    float intensity;
    vec3  direction; uint _dir_pad;
    uint  flags;
    uint  shadow_map_size;
    float shadow_bias;
    float shadow_normal_bias;
    uint  cascade_count;
    // 8 == lux::render::kShadowCascadeSlots (LightDescriptor.hpp). Only the
    // first kMaxShadowCascades (4) ever hold a split; the rest stay zero.
    // Nothing checks this number from here — the tie is gpu_layout_test's
    // DirectionalLightGPU field table, which restates the same 8 on the C++
    // side and would fail on _pad1's offset if the two ever disagreed.
    float cascade_splits[8];
    uint  _pad1[3];
};

struct PointLightGPU {
    vec3  color;     uint _pad0;
    float intensity;
    uint  _intensity_pad[3];
    ivec4 position_page;
    vec3  position_local; uint _pad1;
    float range;
    float attenuation_constant;
    float attenuation_linear;
    float attenuation_quadratic;
    uint  flags;
    uint  shadow_map_size;
    float shadow_bias;
    float shadow_normal_bias;
    uint  _pad2[3];
};

struct SpotLightGPU {
    vec3  color;     uint _pad0;
    float intensity;
    uint  _intensity_pad[3];
    ivec4 position_page;
    vec3  position_local; uint _pad1;
    vec3  direction; uint _pad2;
    float range;
    float attenuation_constant;
    float attenuation_linear;
    float attenuation_quadratic;
    float inner_cone_angle;
    float outer_cone_angle;
    uint  flags;
    uint  shadow_map_size;
    float shadow_bias;
    float shadow_normal_bias;
    uint  _pad3[2];
};

vec3 luxLightScenePosition(PointLightGPU light, float page_size)
{
    return vec3(light.position_page.xyz) * page_size + light.position_local;
}

vec3 luxLightScenePosition(SpotLightGPU light, float page_size)
{
    return vec3(light.position_page.xyz) * page_size + light.position_local;
}

vec3 luxLightViewPosition(
    PointLightGPU light,
    ivec3 camera_page,
    vec3 camera_local,
    float page_size)
{
    return vec3(light.position_page.xyz - camera_page) * page_size
         + light.position_local - camera_local;
}

vec3 luxLightViewPosition(
    SpotLightGPU light,
    ivec3 camera_page,
    vec3 camera_local,
    float page_size)
{
    return vec3(light.position_page.xyz - camera_page) * page_size
         + light.position_local - camera_local;
}

struct AreaLightGPU {
    vec3  color;  uint _pad0;
    float intensity;
    vec2  size;   uint flags;
    uint  shadow_map_size;
    float shadow_bias;
    float shadow_normal_bias;
    uint  _pad1[1];
};

#endif // LIGHT_TYPES_GLSL
