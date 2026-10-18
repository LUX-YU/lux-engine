#ifndef BRDF_GGX_GLSL
#define BRDF_GGX_GLSL
// =========================================================================
//  brdf_ggx.glsl — GGX / Smith microfacet PBR reflectance
//
//  Provides: distributionGGX, geometrySmith, pbrDirectLight.
//  Requires: brdf/brdf_common.glsl included first (PI, fresnelSchlick).
// =========================================================================

// GGX (Trowbridge-Reitz) normal distribution function
float distributionGGX(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Schlick-GGX geometry function (single direction)
float geometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

// Smith geometry function (both view and light directions)
float geometrySmith(float NdotV, float NdotL, float roughness)
{
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

// Full PBR direct-light contribution for a single light source
vec3 pbrDirectLight(vec3 albedo, float metallic, float roughness,
                    vec3 F0, vec3 N, vec3 V, float NdotV,
                    vec3 L, vec3 lightColor)
{
    vec3  H     = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    float NDF = distributionGGX(NdotH, roughness);
    float G   = geometrySmith(NdotV, NdotL, roughness);
    vec3  F   = fresnelSchlick(HdotV, F0);

    vec3  num   = NDF * G * F;
    float denom = 4.0 * NdotV * NdotL + 1e-4;
    vec3  spec  = num / denom;

    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    return (kD * albedo / PI + spec) * lightColor * NdotL;
}

#endif // BRDF_GGX_GLSL
