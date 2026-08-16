#ifndef BRDF_COMMON_GLSL
#define BRDF_COMMON_GLSL
// =========================================================================
//  brdf_common.glsl — shared BRDF constants and utility functions
//
//  Provides: PI, Schlick Fresnel approximation.
//  Include this before any specific BRDF module.
// =========================================================================

const float PI = 3.14159265359;

// ---------------------------------------------------------------------------
// Schlick Fresnel approximation:  F(cosTheta) = F0 + (1 - F0)(1 - cosTheta)^5
// ---------------------------------------------------------------------------
vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

#endif // BRDF_COMMON_GLSL
