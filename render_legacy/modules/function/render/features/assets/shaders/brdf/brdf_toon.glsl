#ifndef BRDF_TOON_GLSL
#define BRDF_TOON_GLSL
// =========================================================================
//  brdf_toon.glsl — Toon / cel-shading quantisation
//
//  Provides: toonStep — quantise a [0,1] value into N discrete bands.
// =========================================================================

float toonStep(float value, float steps)
{
    if (steps <= 0.0) return value;
    return floor(value * steps + 0.5) / steps;
}

// Deferred cel-shading per-light closure. Uses ONLY data available in the
// GBuffer (albedo / normal / roughness) — the rich forward toon params (ramp
// texture, Oren-Nayar sigma, per-material step counts) have no GBuffer storage
// and stay forward-only (fr_stylized.frag). Fixed step counts mirror that
// shader's toonStep flavour: a banded diffuse term + a hard specular band.
// Same per-light signature shape as pbrDirectLight so the deferred dispatcher
// can swap one for the other without touching the light/shadow loop.
vec3 toonDirectLight(vec3 albedo, float roughness, vec3 N, vec3 V, vec3 L, vec3 lightColor)
{
    float NdotL = max(dot(N, L), 0.0);
    float diff  = toonStep(NdotL, 4.0);
    vec3  H     = normalize(L + V);
    float spec  = pow(max(dot(N, H), 0.0), 32.0);
    float specB = toonStep(spec, 2.0);
    return (albedo * diff + vec3(specB)) * lightColor;
}

#endif // BRDF_TOON_GLSL
