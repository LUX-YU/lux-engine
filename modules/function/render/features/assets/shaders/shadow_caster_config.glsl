#ifndef LUX_SHADOW_CASTER_CONFIG_GLSL
#define LUX_SHADOW_CASTER_CONFIG_GLSL

// ShadowResources' compact caster set: binding 2 is the same per-view
// ShadowConfigGPU UBO used by lighting (Light-domain binding 6 after merge).
layout(set = 0, binding = 2, std140) uniform CasterShadowConfigUBO {
    uint  total_slices;
    uint  dir_light_offset;
    uint  dir_cascade_count;
    uint  spot_light_offset;
    uint  spot_light_count;
    uint  point_light_offset;
    uint  point_light_count;
    float dir_split_is_normalized;
    float dir_split_near;
    float dir_split_far;
    uint  dir_caster_slot;
    float scene_time;
} uCasterConfig;

#endif
