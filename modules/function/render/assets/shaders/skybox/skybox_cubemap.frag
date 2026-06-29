#version 450
#extension GL_EXT_nonuniform_qualifier : require

// =============================================================================
// Skybox fragment shader — 6-face cubemap sampling via bindless array.
//
// Uses the bindless cubemap array (set 2, binding 1) with a push constant
// index, matching the engine's GeneralDescriptorSetLayout convention.
// =============================================================================

layout(location = 0) in vec3 vWorldDir;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 1) uniform samplerCube uCubeTex[];

layout(push_constant) uniform PC {
    uint scene_index;
    uint view_index;
    uint skybox_index;   // Bindless index into uCubeTex[]
} uPC;

void main()
{
    outColor = texture(uCubeTex[nonuniformEXT(uPC.skybox_index)], normalize(vWorldDir));
}
