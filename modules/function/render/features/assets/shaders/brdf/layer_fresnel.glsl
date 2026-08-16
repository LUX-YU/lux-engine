#ifndef LAYER_FRESNEL_GLSL
#define LAYER_FRESNEL_GLSL
// =========================================================================
//  layer_fresnel.glsl — Fresnel layer effect
//
//  Adds a view-dependent Fresnel rim contribution on top of the base shading.
//  Requires: brdf/brdf_common.glsl (fresnelSchlick).
//
//  Parameters (from layer_params.x):
//    fresnel_strength — overall intensity of the Fresnel layer
// =========================================================================

/// Compute additive Fresnel layer colour.
/// @param N          World-space normal (normalised)
/// @param V          View direction (normalised)
/// @param strength   Fresnel strength from layer_params.x
/// @param base_f0    Base reflectance (vec3(0.04) for dielectrics, or material-specific)
/// @return Additive colour to blend on top of the base shading result
vec3 fresnelLayerContribution(vec3 N, vec3 V, float strength, vec3 base_f0)
{
    float NdotV = max(dot(N, V), 0.0);
    vec3  F     = fresnelSchlick(NdotV, base_f0);
    return F * strength;
}

#endif // LAYER_FRESNEL_GLSL
