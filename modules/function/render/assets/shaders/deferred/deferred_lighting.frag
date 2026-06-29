#version 450
// =========================================================================
//  deferred_lighting.frag — Deferred lighting pass
//
//  Reads G-Buffer textures and applies PBR lighting with shadow support.
//  World position is reconstructed from depth via inverse view/projection.
//
//  Descriptor sets:
//    Set 0: View (binding 1 = ViewGpuData[])
//    Set 1: G-Buffer inputs (binding 0-3: albedoMetallic, normalRoughness,
//            emissiveAO, depth)
//    Set 2: Light SSBOs (binding 0-3: spot, directional, point, area)
//           + Shadow resources (binding 4-6: slices, atlas, config)
// =========================================================================

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// ========== SET 0: View ==========
struct ViewGpuData {
    mat4 view;
    mat4 proj;
    mat4 inv_view;
    mat4 inv_proj;
    vec4 cam_pos;
    vec4 viewport;
};
layout(set = 0, binding = 1, std430) readonly buffer ViewBuffer {
    ViewGpuData views[];
} uViews;

layout(push_constant) uniform SharedPC {
    uint scene_index;
    uint view_index;
} uPC;

// ========== SET 1: G-Buffer textures ==========
layout(set = 1, binding = 0) uniform sampler2D gAlbedoMetallic;
layout(set = 1, binding = 1) uniform sampler2D gNormalRoughness;
layout(set = 1, binding = 2) uniform sampler2D gEmissiveAo;
layout(set = 1, binding = 3) uniform sampler2D gDepth;

// ========== SET 2: Light SSBOs (same layout as forward set 3) ==========
#include "light_types.glsl"

layout(set = 2, binding = 0, std430) readonly buffer SpotLightBuf {
    SpotLightGPU lights[];
} uSpotLights;

layout(set = 2, binding = 1, std430) readonly buffer DirectionalLightBuf {
    DirectionalLightGPU lights[];
} uDirectionalLights;

layout(set = 2, binding = 2, std430) readonly buffer PointLightBuf {
    PointLightGPU lights[];
} uPointLights;

layout(set = 2, binding = 3, std430) readonly buffer AreaLightBuf {
    AreaLightGPU lights[];
} uAreaLights;

// ========== SET 2: Shadow resources (bindings 4-6) via shared include ==========
// Shadow technique implementation is injected by the SPIR-V variant baker via
// the `LUX_SHADOW_TECHNIQUE_ID` define (glslc does NOT macro-expand the token
// after `#include`, so we branch on an integer id instead of indirecting the
// path string). Defaults to 0 (PCF) so the shader still compiles standalone
// (e.g. when manually invoking glslc for IDE previews).
//   PCF variant  → -DLUX_SHADOW_TECHNIQUE_ID=0
//   EVSM variant → -DLUX_SHADOW_TECHNIQUE_ID=1
#define LUX_LIGHT_SET 2
#ifndef LUX_SHADOW_TECHNIQUE_ID
#define LUX_SHADOW_TECHNIQUE_ID 0
#endif
#if LUX_SHADOW_TECHNIQUE_ID == 0
#include "shadow_pcf.glsl"
#elif LUX_SHADOW_TECHNIQUE_ID == 1
#include "shadow_evsm.glsl"
#else
#error "Unknown LUX_SHADOW_TECHNIQUE_ID"
#endif

// ========== SET 3: Clustered lighting buffers ==========
layout(set = 3, binding = 0, std140) uniform ClusterParamsUBO {
    mat4 view;
    mat4 proj;
    vec4 viewport; // width, height, near, far
    uvec4 grid;    // x, y, z, enable_clustered
    uvec4 limits;  // cluster_count, max_indices, point_count, spot_count
} uClusterParams;

layout(set = 3, binding = 2, std430) readonly buffer ClusterOffsetBuf {
    uint offsets[];
} uClusterOffsets;

layout(set = 3, binding = 4, std430) readonly buffer ClusterIndexBuf {
    uint indices[];
} uClusterIndices;

// ========== Shading via shared includes ==========
#include "brdf/brdf_common.glsl"
#include "brdf/brdf_ggx.glsl"
#include "brdf/brdf_toon.glsl"
#include "gbuffer_encode.glsl"     // octahedral normal + shading-model id (RT1)

// ========== Constants ==========
const float kAmbient = 0.03;

// Per-light shading dispatch on the decoded GBuffer shading-model id. The
// light/shadow/attenuation loop below is model-agnostic and byte-identical to
// the single-model path; only this inner closure call varies. Unlit is handled
// by an early-out in main() (it needs no light loop at all).
vec3 evalShading(uint smId, vec3 albedo, float metallic, float roughness, vec3 F0,
                 vec3 N, vec3 V, float NdotV, vec3 L, vec3 radiance)
{
    if (smId == LUX_SM_TOON)
        return toonDirectLight(albedo, roughness, N, V, L, radiance);
    return pbrDirectLight(albedo, metallic, roughness, F0, N, V, NdotV, L, radiance);
}

// ========== Position reconstruction ==========
vec3 reconstructWorldPos(vec2 uv, float depth)
{
    ViewGpuData v = uViews.views[uPC.view_index];
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 clip = vec4(ndc, depth, 1.0);

    vec4 view_pos = v.inv_proj * clip;
    view_pos.xyz /= view_pos.w;

    vec4 world_pos = v.inv_view * vec4(view_pos.xyz, 1.0);
    return world_pos.xyz;
}

uint computeClusterIndex(vec2 uv, float view_depth)
{
    uint gx = max(uClusterParams.grid.x, 1u);
    uint gy = max(uClusterParams.grid.y, 1u);
    uint gz = max(uClusterParams.grid.z, 1u);

    float width = max(uClusterParams.viewport.x, 1.0);
    float height = max(uClusterParams.viewport.y, 1.0);
    float near_z = max(uClusterParams.viewport.z, 1e-3);
    float far_z = max(uClusterParams.viewport.w, near_z + 1e-3);

    vec2 pixel = vec2(uv.x * width, uv.y * height);
    float tile_w = width / float(gx);
    float tile_h = height / float(gy);

    uint cx = uint(clamp(floor(pixel.x / max(tile_w, 1.0)), 0.0, float(gx - 1u)));
    uint cy = uint(clamp(floor(pixel.y / max(tile_h, 1.0)), 0.0, float(gy - 1u)));

    float z = max(view_depth, near_z);
    float z_norm = log2(z / near_z) / log2(far_z / near_z);
    uint cz = uint(clamp(floor(z_norm * float(gz)), 0.0, float(gz - 1u)));

    return cx + cy * gx + cz * gx * gy;
}

// ========== Main ==========
void main()
{
    // Early-out for sky pixels (depth == 1.0 means no geometry)
    float depth = texture(gDepth, vUV).r;
    if (depth >= 1.0) {
        outColor = vec4(0.0);
        return;
    }

    // Sample G-Buffer
    vec4 albedoMetallic  = texture(gAlbedoMetallic, vUV);
    vec4 normalRoughness = texture(gNormalRoughness, vUV);
    vec4 emissiveAo      = texture(gEmissiveAo, vUV);

    vec3  albedo    = albedoMetallic.rgb;
    float metallic  = albedoMetallic.a;
    vec3  emissive  = emissiveAo.rgb;
    float ao        = emissiveAo.a;

    // RT1 carries the octahedral normal, the shading-model id, and roughness.
    vec3  N;
    uint  smId;
    float roughness;
    unpackGNormalSM(normalRoughness, N, smId, roughness);

    // Unlit: no lighting, no ambient — emit the surface colour + emissive
    // directly. (Replaces the legacy emissive-passthrough hack, which folded
    // base colour into the emissive channel so this PBR pass would surface it.)
    if (smId == LUX_SM_UNLIT) {
        outColor = vec4(albedo + emissive, 1.0);
        return;
    }

    // Reconstruct world position from depth
    vec3 worldPos = reconstructWorldPos(vUV, depth);

    ViewGpuData vd = uViews.views[uPC.view_index];
    vec3 camPos = vd.cam_pos.xyz;
    vec3 V      = normalize(camPos - worldPos);
    float NdotV = max(dot(N, V), 0.0);

    // F0 for dielectric (0.04) or conductor (albedo)
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Compute view depth for cascade selection
    float view_depth = -(vd.view * vec4(worldPos, 1.0)).z;

    // ========== Accumulate lighting ==========
    vec3 Lo = vec3(0.0);

    // --- Directional lights ---
    for (int i = 0; i < uDirectionalLights.lights.length(); ++i) {
        vec3 light_dir = -uDirectionalLights.lights[i].direction;
        float light_dir_len2 = dot(light_dir, light_dir);
        if (light_dir_len2 < 1e-8) continue;
        vec3 L = light_dir * inversesqrt(light_dir_len2);
        vec3 radiance = uDirectionalLights.lights[i].color
                      * uDirectionalLights.lights[i].intensity;
        float shadow = directionalShadow(uint(i), worldPos, N, view_depth);
        Lo += evalShading(smId, albedo, metallic, roughness, F0, N, V, NdotV, L, radiance) * shadow;
    }

    uint point_count = uint(uPointLights.lights.length());
    uint spot_count = uint(uSpotLights.lights.length());

    if (uClusterParams.grid.w != 0u && uClusterParams.grid.x * uClusterParams.grid.y * uClusterParams.grid.z > 0u)
    {
        uint cluster_index = computeClusterIndex(vUV, view_depth);
        if (cluster_index + 1u < uint(uClusterOffsets.offsets.length()))
        {
            uint start = uClusterOffsets.offsets[cluster_index];
            uint end = uClusterOffsets.offsets[cluster_index + 1u];
            uint max_indices = max(uClusterParams.limits.y, 1u);
            end = min(end, max_indices);
            for (uint ii = start; ii < end; ++ii)
            {
                uint packed = uClusterIndices.indices[ii];
                bool is_spot = (packed & 0x80000000u) != 0u;
                uint light_index = packed & 0x7fffffffu;

                if (!is_spot)
                {
                    if (light_index >= point_count) continue;
                    if (uPointLights.lights[light_index].range <= 0.0) continue;
                    vec3  lv = uPointLights.lights[light_index].position - worldPos;
                    float d  = length(lv);
                    if (d <= 1e-5 || d > uPointLights.lights[light_index].range) continue;
                    // Range-windowed falloff: `range` IS the reach. (1-(d/range)^4)^2
                    // smoothly rolls to 0 at range; the constant/linear/quadratic
                    // polynomial is an OPTIONAL extra falloff (default c=1,l=q=0 → 1).
                    float t   = clamp(d / uPointLights.lights[light_index].range, 0.0, 1.0);
                    float win = 1.0 - t * t * t * t; win = win * win;
                    float att = win / (uPointLights.lights[light_index].attenuation_constant +
                                       uPointLights.lights[light_index].attenuation_linear * d +
                                       uPointLights.lights[light_index].attenuation_quadratic * d * d);
                    vec3 radiance = uPointLights.lights[light_index].color
                                  * uPointLights.lights[light_index].intensity * att;
                    float shadow = 1.0;
                    if ((uPointLights.lights[light_index].flags & 1u) != 0u)
                        shadow = pointShadow(light_index, worldPos, N);
                    Lo += evalShading(smId, albedo, metallic, roughness, F0, N, V, NdotV, lv / d, radiance) * shadow;
                }
                else
                {
                    if (light_index >= spot_count) continue;
                    if (dot(uSpotLights.lights[light_index].direction, uSpotLights.lights[light_index].direction) < 1e-8) continue;
                    vec3  lv = uSpotLights.lights[light_index].position - worldPos;
                    float d  = length(lv);
                    if (d <= 1e-5 || d > uSpotLights.lights[light_index].range) continue;

                    vec3 L = lv / d;
                    vec3 spotDir = normalize(uSpotLights.lights[light_index].direction);
                    float theta    = dot(-L, spotDir);
                    float innerCos = cos(uSpotLights.lights[light_index].inner_cone_angle);
                    float outerCos = cos(uSpotLights.lights[light_index].outer_cone_angle);
                    if (theta < outerCos) continue;

                    float spotAtt = clamp((theta - outerCos) / max(innerCos - outerCos, 1e-5), 0.0, 1.0);
                    float t   = clamp(d / uSpotLights.lights[light_index].range, 0.0, 1.0);
                    float win = 1.0 - t * t * t * t; win = win * win;
                    float att = win / (uSpotLights.lights[light_index].attenuation_constant +
                                       uSpotLights.lights[light_index].attenuation_linear * d +
                                       uSpotLights.lights[light_index].attenuation_quadratic * d * d);
                    vec3 radiance = uSpotLights.lights[light_index].color
                                  * uSpotLights.lights[light_index].intensity * att * spotAtt;
                    float shadow = spotShadow(light_index, worldPos, N);
                    Lo += evalShading(smId, albedo, metallic, roughness, F0, N, V, NdotV, L, radiance) * shadow;
                }
            }
        }
    }
    else
    {
        // --- Point lights (fallback path) ---
        for (int i = 0; i < uPointLights.lights.length(); ++i) {
            if (uPointLights.lights[i].range <= 0.0) continue;
            vec3  lv = uPointLights.lights[i].position - worldPos;
            float d  = length(lv);
            if (d <= 1e-5 || d > uPointLights.lights[i].range) continue;
            float t   = clamp(d / uPointLights.lights[i].range, 0.0, 1.0);
            float win = 1.0 - t * t * t * t; win = win * win;
            float att = win / (uPointLights.lights[i].attenuation_constant +
                               uPointLights.lights[i].attenuation_linear * d +
                               uPointLights.lights[i].attenuation_quadratic * d * d);
            vec3 radiance = uPointLights.lights[i].color
                          * uPointLights.lights[i].intensity * att;
            float shadow = 1.0;
            if ((uPointLights.lights[i].flags & 1u) != 0u)
                shadow = pointShadow(uint(i), worldPos, N);
            Lo += evalShading(smId, albedo, metallic, roughness, F0, N, V, NdotV, lv / d, radiance) * shadow;
        }

        // --- Spot lights (fallback path) ---
        for (int i = 0; i < uSpotLights.lights.length(); ++i) {
            if (dot(uSpotLights.lights[i].direction, uSpotLights.lights[i].direction) < 1e-8) continue;
            vec3  lv = uSpotLights.lights[i].position - worldPos;
            float d  = length(lv);
            if (d <= 1e-5 || d > uSpotLights.lights[i].range) continue;

            vec3 L = lv / d;
            vec3 spotDir = normalize(uSpotLights.lights[i].direction);
            float theta    = dot(-L, spotDir);
            float innerCos = cos(uSpotLights.lights[i].inner_cone_angle);
            float outerCos = cos(uSpotLights.lights[i].outer_cone_angle);
            if (theta < outerCos) continue;

            float spotAtt = clamp((theta - outerCos) / max(innerCos - outerCos, 1e-5), 0.0, 1.0);
            float t   = clamp(d / uSpotLights.lights[i].range, 0.0, 1.0);
            float win = 1.0 - t * t * t * t; win = win * win;
            float att = win / (uSpotLights.lights[i].attenuation_constant +
                               uSpotLights.lights[i].attenuation_linear * d +
                               uSpotLights.lights[i].attenuation_quadratic * d * d);
            vec3 radiance = uSpotLights.lights[i].color
                          * uSpotLights.lights[i].intensity * att * spotAtt;
            float shadow = spotShadow(uint(i), worldPos, N);
            Lo += evalShading(smId, albedo, metallic, roughness, F0, N, V, NdotV, L, radiance) * shadow;
        }
    }

    // Ambient + AO
    vec3 ambient = kAmbient * albedo * ao;

    // Final color
    vec3 color = ambient + Lo + emissive;

    outColor = vec4(color, 1.0);
}
