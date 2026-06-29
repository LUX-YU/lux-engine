#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =========================================================================
//  gbuffer_stylized.frag — GBuffer fragment shader for Stylized family
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

layout(constant_id = 0) const bool HAS_NORMAL_MAP = true;
layout(constant_id = 2) const bool HAS_EMISSION   = false;

layout(set = 4, binding = 3, std430) readonly buffer StylizedBuf {
    StylizedMaterialGPU mats[];
} uStylized;

bool hasBit(uint m, uint b) { return (m & (1u << b)) != 0u; }

// Tangent-space normal mapping (shared SSOT; uTex declared above is in scope).
#include "lighting_tbn.glsl"

void main()
{
    uint mat_idx = vMatIndex;
    StylizedMaterialGPU m = uStylized.mats[mat_idx];

    vec3 baseColor = m.base_color.xyz;
    if (hasBit(m.tex_mask, 0u))
        baseColor *= texture(uTex[m.tex[0].bindless], vUV).rgb;

    vec3 N = vWorldNormal;
    if (HAS_NORMAL_MAP && hasBit(m.tex_mask, 2u))
        N = calculateWorldNormal(vWorldNormal, vWorldTangent, vWorldBitangent, vUV, m.tex[2].bindless);
    N = normalize(N);
    // Two-sided lighting: flip the normal written to the G-buffer for back faces
    // (only reached by double_sided materials, cull disabled). No-op for
    // single-sided (gl_FrontFacing always true). Mirrors gbuffer_pbr.
    if (!gl_FrontFacing)
        N = -N;

    vec3 emissive = m.emissive.xyz * m.emissive.w;
    if (HAS_EMISSION && hasBit(m.tex_mask, 3u))
        emissive *= texture(uTex[m.tex[3].bindless], vUV).rgb;

    gAlbedoMetallic  = vec4(baseColor, 0.0);
    gNormalRoughness = packGNormalSM(N, LUX_SM_TOON, 0.5);
    gEmissiveAo      = vec4(emissive, 1.0);
}
