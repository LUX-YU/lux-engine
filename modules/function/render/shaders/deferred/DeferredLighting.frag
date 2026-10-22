#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "gbuffer_encode.glsl"
#include "brdf/brdf_common.glsl"
#include "brdf/brdf_ggx.glsl"
#include "brdf/direct_lighting.glsl"
#ifdef CLUSTERED
#include "ClusterLookup.glsl"
#endif
#ifdef SHADOWS
#include "../shadow/ShadowPcf.glsl"
#endif

layout(set = 0, binding = 0) uniform texture2D albedo_metallic;
layout(set = 0, binding = 1) uniform texture2D normal_roughness;
layout(set = 0, binding = 2) uniform texture2D emissive_ao;
layout(set = 0, binding = 3) uniform texture2D depth;

struct DirectionalLight { vec4 radiance; vec4 direction; };
struct PointLight { vec4 position_range; vec4 radiance; vec4 attenuation; };
struct SpotLight { vec4 position_range; vec4 radiance; vec4 direction_outer; vec4 attenuation_inner; };
layout(set = 1, binding = 0, std430) readonly buffer DirectionalLights { DirectionalLight values[]; } directional_lights;
layout(set = 1, binding = 1, std430) readonly buffer PointLights { PointLight values[]; } point_lights;
layout(set = 1, binding = 2, std430) readonly buffer SpotLights { SpotLight values[]; } spot_lights;
layout(set = 1, binding = 3, std430) readonly buffer LightCounts { uvec4 values; } light_counts;

layout(push_constant) uniform Parameters
{
    mat4 inverse_view_projection;
    vec4 camera_position;
    vec4 viewport;
    vec4 background;
    vec4 environment;
} parameters;

layout(location = 0) out vec4 hdr_color;

vec3 evaluateLocalLight(uint packed, vec3 position, vec3 n, vec3 view, float ndotv,
                        vec3 albedo, float metallic, float roughness, vec3 f0)
{
    uint index = packed & 0x7fffffffu;
    bool spot = (packed & 0x80000000u) != 0;
    vec4 position_range, radiance, attenuation;
    vec4 direction_outer = vec4(0);
    if (spot)
    {
        SpotLight light = spot_lights.values[index];
        position_range = light.position_range; radiance = light.radiance;
        attenuation = light.attenuation_inner; direction_outer = light.direction_outer;
    }
    else
    {
        PointLight light = point_lights.values[index];
        position_range = light.position_range; radiance = light.radiance; attenuation = light.attenuation;
    }
    vec3 delta = position_range.xyz - position;
    float distanceToLight = length(delta);
    if (distanceToLight <= 1e-5 || distanceToLight >= position_range.w) return vec3(0);
    vec3 direction = delta / distanceToLight;
    float cone = spot ? smoothstep(direction_outer.w, attenuation.w, dot(-direction, direction_outer.xyz)) : 1;
    vec3 incident = radiance.rgb * cone * rangeAttenuation(distanceToLight, position_range.w, attenuation.xyz);
#ifdef SHADOWS
    incident *= shadowVisibility(spot ? 2 : 1, index, position, n, position_range.xyz);
#endif
    return pbrDirectLight(albedo, metallic, roughness, f0, n, view, ndotv, direction, incident);
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float d = texelFetch(depth, pixel, 0).r;
    if (d >= 1.0)
    {
        hdr_color = vec4(parameters.background.rgb, 1.0);
        return;
    }
    vec4 a = texelFetch(albedo_metallic, pixel, 0);
    vec4 e = texelFetch(emissive_ao, pixel, 0);
    vec3 n;
    uint model;
    float roughness;
    unpackGNormalSM(texelFetch(normal_roughness, pixel, 0), n, model, roughness);
    if (model == LUX_SM_UNLIT)
    {
        hdr_color = vec4(a.rgb + e.rgb, 1.0);
        return;
    }
    // The active NRI viewport uses positive height (originBottomLeft=true). Convert once, here.
    vec2 uv = (gl_FragCoord.xy / vec2(textureSize(depth, 0)) - parameters.viewport.xy) / parameters.viewport.zw;
    vec4 reconstructed = parameters.inverse_view_projection * vec4(uv * 2.0 - 1.0, d, 1.0);
    vec3 position = reconstructed.xyz / reconstructed.w;
    vec3 view = normalize(parameters.camera_position.xyz - position);
    float ndotv = max(dot(n, view), 0.0);
    float metallic = clamp(a.a, 0.0, 1.0);
    roughness = clamp(roughness, 0.04, 1.0);
    vec3 f0 = mix(vec3(0.04), a.rgb, metallic);
    vec3 color = e.rgb + parameters.environment.rgb * a.rgb * (1.0 - metallic) * clamp(e.a, 0.0, 1.0);
    for (uint i = 0; i < light_counts.values.x; ++i)
    {
        DirectionalLight light = directional_lights.values[i];
        vec3 radiance = light.radiance.rgb;
#ifdef SHADOWS
        radiance *= shadowVisibility(0, i, position, n, vec3(0));
#endif
        color += pbrDirectLight(a.rgb, metallic, roughness, f0, n, view, ndotv, -light.direction.xyz, radiance);
    }
#ifdef CLUSTERED
    uint first, end;
    if (clusterRange(position, first, end))
    {
        for (uint at = first; at < end; ++at)
            color += evaluateLocalLight(cluster_indices.values[at], position, n, view, ndotv, a.rgb, metallic, roughness, f0);
    }
    else
#endif
    {
        for (uint i = 0; i < light_counts.values.y; ++i)
            color += evaluateLocalLight(i, position, n, view, ndotv, a.rgb, metallic, roughness, f0);
        for (uint i = 0; i < light_counts.values.z; ++i)
            color += evaluateLocalLight(i | 0x80000000u, position, n, view, ndotv, a.rgb, metallic, roughness, f0);
    }
    hdr_color = vec4(color, 1.0);
}
