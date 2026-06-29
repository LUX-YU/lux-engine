#version 450
#extension GL_EXT_nonuniform_qualifier : require

// =============================================================================
// Skybox fragment shader — equirectangular (panoramic) texture sampling.
//
// Uses the standard bindless texture array (set 2, binding 0) with a push
// constant index, matching the engine's GeneralDescriptorSetLayout convention.
// =============================================================================

layout(location = 0) in vec3 vWorldDir;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D uTex[];

layout(push_constant) uniform PC {
    uint scene_index;
    uint view_index;
    uint skybox_index;   // Bindless index into uTex[]
} uPC;

const float INV_PI  = 0.3183098861;
const float INV_2PI = 0.1591549431;

void main()
{
    vec3 dir = normalize(vWorldDir);

    // Spherical coordinates → equirectangular UV
    float phi   = atan(dir.z, dir.x);
    float theta = asin(clamp(dir.y, -1.0, 1.0));
    vec2 uv = vec2(phi * INV_2PI + 0.5, theta * INV_PI + 0.5);

    outColor = texture(uTex[nonuniformEXT(uPC.skybox_index)], uv);
}
