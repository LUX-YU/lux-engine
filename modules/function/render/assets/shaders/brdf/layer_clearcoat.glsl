#ifndef LAYER_CLEARCOAT_GLSL
#define LAYER_CLEARCOAT_GLSL
// =========================================================================
//  layer_clearcoat.glsl — Clearcoat layer (thin dielectric coat on top)
//
//  Adds a secondary GGX specular lobe at the clearcoat roughness level.
//  Requires: brdf/brdf_common.glsl, brdf/brdf_ggx.glsl
//
//  Parameters (from layer_params):
//    clearcoat_factor    — y component (0 = no clearcoat, 1 = full)
//    clearcoat_roughness — z component
// =========================================================================

/// Compute clearcoat specular for one light direction.
/// @param N             Surface normal
/// @param V             View direction
/// @param L             Light direction
/// @param factor        Clearcoat factor (layer_params.y)
/// @param cc_roughness  Clearcoat roughness (layer_params.z)
/// @return Specular contribution from clearcoat lobe
vec3 clearcoatSpecular(vec3 N, vec3 V, vec3 L, float factor, float cc_roughness)
{
    vec3  H     = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    // Clearcoat uses fixed F0 = 0.04 (air-dielectric interface)
    vec3  F_cc   = fresnelSchlick(HdotV, vec3(0.04));
    float D_cc   = distributionGGX(NdotH, max(cc_roughness, 0.04));
    float G_cc   = geometrySmith(NdotV, NdotL, max(cc_roughness, 0.04));

    float denom = 4.0 * NdotV * NdotL + 1e-4;
    vec3  spec  = (D_cc * G_cc * F_cc) / denom;

    return spec * factor * NdotL;
}

#endif // LAYER_CLEARCOAT_GLSL
