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
    float cascade_splits[8];
    uint  _pad1[3];
};

struct PointLightGPU {
    vec3  color;     uint _pad0;
    float intensity;
    vec3  position;  uint _pad1;
    float range;
    float attenuation_constant;
    float attenuation_linear;
    float attenuation_quadratic;
    uint  flags;
    uint  shadow_map_size;
    float shadow_bias;
    float shadow_normal_bias;
    uint  _pad2[1];
};

struct SpotLightGPU {
    vec3  color;     uint _pad0;
    float intensity;
    vec3  position;  uint _pad1;
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
};

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
