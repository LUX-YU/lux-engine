#ifndef BRDF_OREN_NAYAR_GLSL
#define BRDF_OREN_NAYAR_GLSL
// =========================================================================
//  brdf_oren_nayar.glsl — Oren-Nayar diffuse reflectance model
//
//  Provides: orenNayarDiffuse — rough-surface Lambertian extension.
//  sigma_rad is the roughness parameter in radians.
// =========================================================================

float orenNayarDiffuse(vec3 L, vec3 V, vec3 N, float sigma_rad)
{
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    float theta_r = acos(NdotV);  // angle of view
    float theta_i = acos(NdotL);  // angle of incidence

    float sigma2 = sigma_rad * sigma_rad;
    float A = 1.0 - 0.5 * sigma2 / (sigma2 + 0.33);
    float B = 0.45 * sigma2 / (sigma2 + 0.09);

    // max(0, cos(phi_r - phi_i)) — project V and L onto the tangent plane
    vec3 vPerp = normalize(V - NdotV * N);
    vec3 lPerp = normalize(L - NdotL * N);
    float cosPhiDiff = max(dot(vPerp, lPerp), 0.0);

    float alpha = max(theta_r, theta_i);
    float beta  = min(theta_r, theta_i);

    return NdotL * (A + B * cosPhiDiff * sin(alpha) * tan(beta));
}

#endif // BRDF_OREN_NAYAR_GLSL
