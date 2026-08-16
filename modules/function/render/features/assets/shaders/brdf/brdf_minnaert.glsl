#ifndef BRDF_MINNAERT_GLSL
#define BRDF_MINNAERT_GLSL
// =========================================================================
//  brdf_minnaert.glsl — Minnaert limb-darkening reflectance model
//
//  Provides: minnaertReflectance — `(NdotL)^(1+k) * (NdotV)^(1-k)`
//  where k controls limb darkening (0 = Lambertian, >0 = darker edges).
// =========================================================================

float minnaertReflectance(float NdotL, float NdotV, float k)
{
    return max(pow(max(NdotL, 0.0), 1.0 + k) * pow(max(NdotV, 0.0), 1.0 - k), 0.0);
}

#endif // BRDF_MINNAERT_GLSL
