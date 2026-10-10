#ifndef LAYER_SHEEN_GLSL
#define LAYER_SHEEN_GLSL
// =========================================================================
//  layer_sheen.glsl — Sheen layer (cloth-like soft highlight)
//
//  Approximates the Charlie sheen model (inverse Gaussian on sin(theta)).
//  Requires: brdf/brdf_common.glsl (PI)
//
//  Parameters:
//    sheen_roughness — from layer_params.w
//    sheen_color     — from layer_colors.xyz
// =========================================================================

/// Approximate Charlie sheen distribution.
float sheenDistribution(float NdotH, float roughness)
{
    float inv_r = 1.0 / max(roughness, 0.01);
    float sin2  = 1.0 - NdotH * NdotH;
    return (2.0 + inv_r) * pow(max(sin2, 0.0), inv_r * 0.5) / (2.0 * PI);
}

/// Compute sheen contribution for one light direction.
/// @param N              Surface normal
/// @param V              View direction
/// @param L              Light direction
/// @param sheen_color    Tint colour (layer_colors.xyz)
/// @param sheen_rough    Roughness (layer_params.w)
/// @return Additive sheen specular colour per light
vec3 sheenSpecular(vec3 N, vec3 V, vec3 L, vec3 sheen_color, float sheen_rough)
{
    vec3  H     = normalize(V + L);
    float NdotH = max(dot(N, H), 0.0);
    float NdotL = max(dot(N, L), 0.0);

    float D = sheenDistribution(NdotH, sheen_rough);
    return sheen_color * D * NdotL;
}

#endif // LAYER_SHEEN_GLSL
