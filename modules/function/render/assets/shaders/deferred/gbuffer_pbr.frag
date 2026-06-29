#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =========================================================================
//  gbuffer_pbr.frag — GBuffer fragment shader for PBR family
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
layout(constant_id = 3) const bool ALPHA_CUTOUT   = false;
layout(constant_id = 7) const bool HAS_AO_MAP     = false;
layout(constant_id = 8) const bool HAS_METALLIC   = false;

layout(set = 4, binding = 2, std430) readonly buffer PbrBuf {
    PbrMaterialGPU mats[];
} uPbr;

bool hasBit(uint m, uint b) { return (m & (1u << b)) != 0u; }

// Tangent-space normal mapping (shared SSOT; uTex declared above is in scope).
#include "lighting_tbn.glsl"

void main()
{
    uint mat_idx = vMatIndex;
    PbrMaterialGPU m = uPbr.mats[mat_idx];

    uint texMask = m.tex_mask;

    vec4 baseColor = m.base_color;
    if (hasBit(texMask, 0u))
        baseColor *= texture(uTex[m.tex[0].bindless], vUV);

    uint alphaMode = floatBitsToUint(m.alpha_params.x);
    if (ALPHA_CUTOUT && alphaMode == 1u && baseColor.a < m.alpha_params.y)
        discard;

    float metallic  = m.pbr_params.x;
    float roughness = m.pbr_params.y;
    if (HAS_METALLIC && hasBit(texMask, 1u)) {
        vec4 mr = texture(uTex[m.tex[1].bindless], vUV);
        metallic  *= mr.b;
        roughness *= mr.g;
    }
    roughness = max(roughness, 0.04);

    vec3 N = vWorldNormal;
    if (HAS_NORMAL_MAP && hasBit(texMask, 2u))
        N = calculateWorldNormal(vWorldNormal, vWorldTangent, vWorldBitangent, vUV, m.tex[2].bindless);
    N = normalize(N);
    // Two-sided lighting: a back-facing fragment only occurs for double_sided
    // materials (rendered with cull disabled). Flip the normal written to the
    // G-buffer so the deferred lighting pass shades/shadows the side facing the
    // viewer — otherwise the underside mirrors the lit top, including its shadow.
    // gl_FrontFacing is always true for culled single-sided materials (no-op).
    if (!gl_FrontFacing)
        N = -N;

    float ao = 1.0;
    if (HAS_AO_MAP && hasBit(texMask, 3u))
        ao = texture(uTex[m.tex[3].bindless], vUV).r;

    vec3 emissive = m.emissive.xyz * m.emissive.w;
    if (HAS_EMISSION && hasBit(texMask, 4u))
        emissive *= texture(uTex[m.tex[4].bindless], vUV).rgb;

    gAlbedoMetallic  = vec4(baseColor.rgb, metallic);
    gNormalRoughness = packGNormalSM(N, LUX_SM_PBR, roughness);
    gEmissiveAo      = vec4(emissive, ao);
}
