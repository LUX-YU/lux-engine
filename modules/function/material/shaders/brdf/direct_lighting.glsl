#ifndef LUX_DIRECT_LIGHTING_GLSL
#define LUX_DIRECT_LIGHTING_GLSL

// Legacy finite-range window, with an explicit positive attenuation contract at admission.
float rangeAttenuation(float distanceToLight, float range, vec3 coefficients)
{
    float q = distanceToLight / range;
    float window = max(1.0 - q * q * q * q, 0.0);
    return window * window / dot(coefficients, vec3(1.0, distanceToLight, distanceToLight * distanceToLight));
}
#endif
