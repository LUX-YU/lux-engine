#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =========================================================================
//  gbuffer_unlit.frag — GBuffer fragment shader for Unlit family
// =========================================================================

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vWorldNormal;
layout(location = 2) in vec3 vViewDir;
layout(location = 3) in vec2 vUV;
layout(location = 4) in flat uint vMatIndex;
layout(location = 5) in vec3 vWorldTangent;
layout(location = 6) in vec3 vWorldBitangent;

layout(location = 0) out vec4 gAlbedoMetallic;
layout(location = 1) out vec4 gNormalRoughness;
layout(location = 2) out vec4 gEmissiveAo;

layout(set = 2, binding = 0) uniform sampler2D uTex[];

#include "material_types.glsl"
#include "gbuffer_encode.glsl"

layout(constant_id = 2) const bool HAS_EMISSION = false;

layout(set = 4, binding = 0, std430) readonly buffer UnlitBuf {
    UnlitMaterialGPU mats[];
} uUnlit;

bool hasBit(uint m, uint b) { return (m & (1u << b)) != 0u; }

void main()
{
    uint mat_idx = vMatIndex;
    UnlitMaterialGPU m = uUnlit.mats[mat_idx];

    vec4 baseColor = m.base_color;
    if (hasBit(m.tex_mask, 0u))
        baseColor *= texture(uTex[m.tex[0].bindless], vUV);

    vec3 emissive = m.emissive.xyz * m.emissive.w;
    if (HAS_EMISSION && hasBit(m.tex_mask, 1u))
        emissive *= texture(uTex[m.tex[1].bindless], vUV).rgb;

    // Phase D: write a real Unlit shading-model id; the deferred pass dispatches
    // to a no-lighting branch (albedo + emissive). This RETIRES the legacy
    // emissive-passthrough hack (base color was folded into gEmissiveAo so the
    // PBR-only lighting pass would surface it). Base color now stays in albedo.
    gAlbedoMetallic  = vec4(baseColor.rgb, 0.0);
    gNormalRoughness = packGNormalSM(normalize(vWorldNormal), LUX_SM_UNLIT, 1.0);
    gEmissiveAo      = vec4(emissive, 1.0);
}
