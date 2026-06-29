#version 450
#extension GL_EXT_nonuniform_qualifier : require
// =========================================================================
//  fr_pbr.frag — Forward fragment shader for PBR family (Family 2)
//
//  Matches PbrFamilyGPU (C++ side).
//  GGX / Smith microfacet BRDF with optional layer support.
//
//  Shading models:
//    200 = PbrMetallicRoughness — Standard metallic-roughness (glTF PBR)
//
//  Layer support (compile-time via specialization constants):
//    HAS_FRESNEL_LAYER (16), HAS_CLEARCOAT (14), HAS_SHEEN (15)
// =========================================================================
#include "lighting_common.glsl"
// Shadow technique selected at SPIR-V variant build time via
// LUX_SHADOW_TECHNIQUE_ID (PCF=0, EVSM=1, future MSM=2, VSM=3 ...).
// glslc doesn't macro-expand the token after `#include`, hence the integer
// switch instead of indirecting the path string.
#ifndef LUX_SHADOW_TECHNIQUE_ID
#define LUX_SHADOW_TECHNIQUE_ID 0
#endif
#if LUX_SHADOW_TECHNIQUE_ID == 0
#include "shadow_pcf.glsl"
#elif LUX_SHADOW_TECHNIQUE_ID == 1
#include "shadow_evsm.glsl"
#else
#error "Unknown LUX_SHADOW_TECHNIQUE_ID"
#endif
#include "brdf/brdf_common.glsl"
#include "brdf/brdf_ggx.glsl"
#include "brdf/layer_fresnel.glsl"
#include "brdf/layer_clearcoat.glsl"
#include "brdf/layer_sheen.glsl"

// ========== Specialization Constants ==========
layout(constant_id = 0)  const bool HAS_NORMAL_MAP     = true;
layout(constant_id = 2)  const bool HAS_EMISSION       = false;
layout(constant_id = 3)  const bool ALPHA_CUTOUT        = false;
layout(constant_id = 7)  const bool HAS_AO_MAP          = false;
layout(constant_id = 8)  const bool HAS_METALLIC        = false;
layout(constant_id = 14) const bool HAS_CLEARCOAT       = false;
layout(constant_id = 15) const bool HAS_SHEEN           = false;
layout(constant_id = 16) const bool HAS_FRESNEL_LAYER   = false;

// ========== SET 4: PBR Family Material (binding 2) ==========
layout(set = 4, binding = 2, std430) readonly buffer PbrBuf {
    PbrMaterialGPU mats[];
} uPbr;

// =========================================================================
//  Main
// =========================================================================
void main()
{
    uint matIdx = vMatIndex;
    PbrMaterialGPU m = uPbr.mats[matIdx];
    uint texMask = m.tex_mask;

    // --- Base colour ---
    vec4 baseColor = m.base_color;
    if (hasBit(texMask, 0u))
        baseColor *= texture(uTex[m.tex[0].bindless], vUV);

    // --- Alpha cutout ---
    uint alphaMode = floatBitsToUint(m.alpha_params.x);
    if (ALPHA_CUTOUT && alphaMode == 1u && baseColor.a < m.alpha_params.y)
        discard;

    // --- Metallic / roughness ---
    float metallic  = m.pbr_params.x;
    float roughness = m.pbr_params.y;
    if (HAS_METALLIC && hasBit(texMask, 1u)) {
        vec4 mr = texture(uTex[m.tex[1].bindless], vUV);
        metallic  *= mr.b;
        roughness *= mr.g;
    }
    roughness = max(roughness, 0.04);

    // --- Normal ---
    vec3 N = vWorldNormal;
    if (HAS_NORMAL_MAP && hasBit(texMask, 2u))
        N = calculateWorldNormal(vWorldNormal, vWorldTangent, vWorldBitangent, vUV, m.tex[2].bindless);
    N = normalize(N);
    // Two-sided lighting: a back-facing fragment only occurs for double_sided
    // materials (rendered with cull disabled). Flip the normal so the shaded
    // side faces the viewer — otherwise the underside mirrors the lit top,
    // including its shadow. For culled single-sided materials gl_FrontFacing is
    // always true, so this is a no-op (zero change to existing behaviour).
    if (!gl_FrontFacing)
        N = -N;

    vec3  albedo = baseColor.rgb;
    vec3  V      = normalize(vViewDir);
    float NdotV  = max(dot(N, V), 0.0);

    // Dielectric F0 = 0.04; conductor F0 = albedo
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // --- GGX / Smith lighting accumulation ---
    vec3 Lo = vec3(0.0);
    float view_depth = computeViewDepth(vWorldPos);

    for (int i = 0; i < uDirectionalLights.lights.length(); ++i) {
        vec3 Ldir = uDirectionalLights.lights[i].direction;
        if (dot(Ldir, Ldir) < 1e-8) continue;
        vec3 L = normalize(-Ldir);
        vec3 lc = uDirectionalLights.lights[i].color * uDirectionalLights.lights[i].intensity;
        float shadow = directionalShadow(uint(i), vWorldPos, N, view_depth);

        vec3 contrib = pbrDirectLight(albedo, metallic, roughness, F0, N, V, NdotV, L, lc);

        // Layer contributions (per-light)
        if (HAS_CLEARCOAT)
            contrib += clearcoatSpecular(N, V, L, m.layer_params.y, m.layer_params.z) * lc;
        if (HAS_SHEEN)
            contrib += sheenSpecular(N, V, L, m.layer_colors.xyz, m.layer_params.w) * lc;

        Lo += contrib * shadow;
    }

    for (int i = 0; i < uPointLights.lights.length(); ++i) {
        if (uPointLights.lights[i].range <= 0.0) continue;
        vec3  lv = uPointLights.lights[i].position - vWorldPos;
        float d  = length(lv);
        if (d > uPointLights.lights[i].range) continue;
        vec3  L = lv / d;
        float att = 1.0 / (uPointLights.lights[i].attenuation_constant +
                           uPointLights.lights[i].attenuation_linear * d +
                           uPointLights.lights[i].attenuation_quadratic * d * d);
        vec3 lc = uPointLights.lights[i].color * uPointLights.lights[i].intensity * att;

        float shadow = 1.0;
        if ((uPointLights.lights[i].flags & 1u) != 0u)
            shadow = pointShadow(uint(i), vWorldPos, N);

        vec3 contrib = pbrDirectLight(albedo, metallic, roughness, F0, N, V, NdotV, L, lc);

        if (HAS_CLEARCOAT)
            contrib += clearcoatSpecular(N, V, L, m.layer_params.y, m.layer_params.z) * lc;
        if (HAS_SHEEN)
            contrib += sheenSpecular(N, V, L, m.layer_colors.xyz, m.layer_params.w) * lc;

        Lo += contrib * shadow;
    }

    // --- Ambient + AO ---
    float ao = 1.0;
    if (HAS_AO_MAP && hasBit(texMask, 3u))
        ao = texture(uTex[m.tex[3].bindless], vUV).r;
    vec3 ambient = kAmbient * albedo * mix(1.0, ao, m.pbr_params.w);

    // --- Emissive ---
    vec3 emissiveColor = m.emissive.xyz * m.emissive.w;
    if (HAS_EMISSION && hasBit(texMask, 4u))
        emissiveColor *= texture(uTex[m.tex[4].bindless], vUV).rgb;

    // --- Fresnel layer (view-dependent, not per-light) ---
    vec3 fresnelLayer = vec3(0.0);
    if (HAS_FRESNEL_LAYER)
        fresnelLayer = fresnelLayerContribution(N, V, m.layer_params.x, F0);

    vec3 color = ambient + Lo + emissiveColor + fresnelLayer;
    outColor = vec4(color, baseColor.a);
}
