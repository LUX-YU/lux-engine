#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =========================================================================
//  fr_unlit.frag — Forward fragment shader for Unlit family (Family 0)
//
//  Matches UnlitFamilyGPU (C++ side).  No lighting — outputs base colour
//  and optional emissive directly.
//
//  Specialization constants:
//    2 = HAS_EMISSION
// =========================================================================
#include "lighting_common.glsl"

// ========== Specialization Constants ==========
layout(constant_id = 2) const bool HAS_EMISSION = false;

// ========== SET 4: Unlit Family Material (binding 0) ==========
layout(set = 4, binding = 0, std430) readonly buffer UnlitBuf {
    UnlitMaterialGPU mats[];
} uUnlit;

// =========================================================================
//  Main
// =========================================================================
void main()
{
    uint matIdx = vMatIndex;
    UnlitMaterialGPU m = uUnlit.mats[matIdx];

    vec4 color = m.base_color;
    if (hasBit(m.tex_mask, 0u))
        color *= texture(uTex[m.tex[0].bindless], vUV);

    vec3 emissive = m.emissive.xyz * m.emissive.w;
    if (HAS_EMISSION && hasBit(m.tex_mask, 1u))
        emissive *= texture(uTex[m.tex[1].bindless], vUV).rgb;

    outColor = vec4(color.rgb + emissive, color.a);
}
