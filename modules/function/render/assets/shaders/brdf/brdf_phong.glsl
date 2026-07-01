#ifndef BRDF_PHONG_GLSL
#define BRDF_PHONG_GLSL
// =========================================================================
//  brdf_phong.glsl — Phong / Blinn-Phong reflectance model
//
//  Provides per-light calculators and a full lighting accumulation function.
//  Requires: lighting_common.glsl included first (light SSBO declarations,
//            kAmbient, vWorldPos, vViewDir).
// =========================================================================

// ========== Per-light reflectance ==========

vec3 calculateDirectionalLight(DirectionalLightGPU light, vec3 normal, vec3 viewDir, vec3 kd, vec3 ks, float shininess)
{
    vec3 L = normalize(-light.direction);
    vec3 N = normalize(normal);
    vec3 V = normalize(viewDir);
    vec3 R = normalize(reflect(-L, N));

    float NdotL = max(dot(N, L), 0.0);
    vec3 lightColor = light.color * light.intensity;

    vec3 diffuse = kd * lightColor * NdotL;
    float specPow = pow(max(dot(R, V), 0.0), max(1.0, shininess));
    vec3 specular = ks * lightColor * specPow;

    return diffuse + specular;
}

vec3 calculatePointLight(PointLightGPU light, vec3 worldPos, vec3 normal, vec3 viewDir, vec3 kd, vec3 ks, float shininess)
{
    vec3 lightDir = light.position - worldPos;
    float distance = length(lightDir);

    if (distance > light.range) {
        return vec3(0.0);
    }

    lightDir = normalize(lightDir);
    vec3 N = normalize(normal);
    vec3 V = normalize(viewDir);
    vec3 R = normalize(reflect(-lightDir, N));

    float attenuation = 1.0 / (light.attenuation_constant +
                              light.attenuation_linear * distance +
                              light.attenuation_quadratic * distance * distance);

    float NdotL = max(dot(N, lightDir), 0.0);
    vec3 lightColor = light.color * light.intensity * attenuation;

    vec3 diffuse = kd * lightColor * NdotL;
    float specPow = pow(max(dot(R, V), 0.0), max(1.0, shininess));
    vec3 specular = ks * lightColor * specPow;

    return diffuse + specular;
}

vec3 calculateSpotLight(SpotLightGPU light, vec3 worldPos, vec3 normal, vec3 viewDir, vec3 kd, vec3 ks, float shininess)
{
    vec3 lightDir = light.position - worldPos;
    float distance = length(lightDir);

    if (distance > light.range) {
        return vec3(0.0);
    }

    lightDir = normalize(lightDir);

    vec3 spotDir = normalize(light.direction);
    float theta = dot(-lightDir, spotDir);
    float innerCos = cos(light.inner_cone_angle);
    float outerCos = cos(light.outer_cone_angle);

    if (theta < outerCos) {
        return vec3(0.0);
    }

    vec3 N = normalize(normal);
    vec3 V = normalize(viewDir);
    vec3 R = normalize(reflect(-lightDir, N));

    float attenuation = 1.0 / (light.attenuation_constant +
                              light.attenuation_linear * distance +
                              light.attenuation_quadratic * distance * distance);

    float intensity = clamp((theta - outerCos) / (innerCos - outerCos), 0.0, 1.0);

    float NdotL = max(dot(N, lightDir), 0.0);
    vec3 lightColor = light.color * light.intensity * attenuation * intensity;

    vec3 diffuse = kd * lightColor * NdotL;
    float specPow = pow(max(dot(R, V), 0.0), max(1.0, shininess));
    vec3 specular = ks * lightColor * specPow;

    return diffuse + specular;
}

// ========== Full Phong lighting accumulation ==========
// Call after extracting kd/ks/shininess and applying texture + normal mapping.
vec3 accumulateLighting(vec3 worldPos, vec3 normal, vec3 viewDir, vec3 kd, vec3 ks, float shininess)
{
    vec3 ambient = kAmbient * kd;
    vec3 totalLighting = vec3(0.0);

    for (int i = 0; i < uDirectionalLights.lights.length(); ++i) {
        if (dot(uDirectionalLights.lights[i].direction, uDirectionalLights.lights[i].direction) < 1e-8)
            continue;
        totalLighting += calculateDirectionalLight(uDirectionalLights.lights[i], normal, viewDir, kd, ks, shininess);
    }

    for (int i = 0; i < uPointLights.lights.length(); ++i) {
        if (uPointLights.lights[i].range <= 0.0) continue;
        totalLighting += calculatePointLight(uPointLights.lights[i], worldPos, normal, viewDir, kd, ks, shininess);
    }

    for (int i = 0; i < uSpotLights.lights.length(); ++i) {
        if (dot(uSpotLights.lights[i].direction, uSpotLights.lights[i].direction) < 1e-8)
            continue;
        totalLighting += calculateSpotLight(uSpotLights.lights[i], worldPos, normal, viewDir, kd, ks, shininess);
    }

    return ambient + totalLighting;
}

// ========== Shadow-aware Phong lighting accumulation ==========
// Available when shadow_common.glsl is included before this file.
#ifdef SHADOW_ENABLED
vec3 accumulateLightingShadow(vec3 worldPos, vec3 normal, vec3 viewDir, vec3 kd, vec3 ks, float shininess, float view_depth)
{
    vec3 ambient = kAmbient * kd;
    vec3 totalLighting = vec3(0.0);

    for (int i = 0; i < uDirectionalLights.lights.length(); ++i) {
        if (dot(uDirectionalLights.lights[i].direction, uDirectionalLights.lights[i].direction) < 1e-8)
            continue;
        float shadow = directionalShadow(uint(i), worldPos, normal, view_depth);
        totalLighting += calculateDirectionalLight(uDirectionalLights.lights[i], normal, viewDir, kd, ks, shininess) * shadow;
    }

    for (int i = 0; i < uPointLights.lights.length(); ++i) {
        if (uPointLights.lights[i].range <= 0.0) continue;
        float shadow = 1.0;
        if ((uPointLights.lights[i].flags & 1u) != 0u)
            shadow = pointShadow(uint(i), worldPos, normal);
        totalLighting += calculatePointLight(uPointLights.lights[i], worldPos, normal, viewDir, kd, ks, shininess) * shadow;
    }

    for (int i = 0; i < uSpotLights.lights.length(); ++i) {
        if (dot(uSpotLights.lights[i].direction, uSpotLights.lights[i].direction) < 1e-8)
            continue;
        float shadow = spotShadow(uint(i), worldPos, normal);
        totalLighting += calculateSpotLight(uSpotLights.lights[i], worldPos, normal, viewDir, kd, ks, shininess) * shadow;
    }

    return ambient + totalLighting;
}
#endif

#endif // BRDF_PHONG_GLSL
