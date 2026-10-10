#ifndef LIGHTING_TBN_GLSL
#define LIGHTING_TBN_GLSL
// =========================================================================
//  lighting_tbn.glsl — tangent-space normal-map → world-space normal.
//
//  Single source of truth shared by the forward path (via lighting_common.glsl)
//  and the deferred GBuffer frags (gbuffer_pbr / gbuffer_stylized), which all
//  previously hand-copied this identical function.
//
//  Requires: the bindless texture array `uTex[]` (set 2, binding 0) in scope at
//  the include point (declare it BEFORE including this file).
// =========================================================================

vec3 calculateWorldNormal(vec3 worldNormal, vec3 worldTangent, vec3 worldBitangent,
                          vec2 uv, TextureRefGPU normalTexture)
{
    mat3 TBN = mat3(
        normalize(worldTangent),
        normalize(worldBitangent),
        normalize(worldNormal)
    );
    vec3 nm = luxSampleTexture(normalTexture, uv).rgb * 2.0 - 1.0;
    return normalize(TBN * nm);
}

#endif // LIGHTING_TBN_GLSL
