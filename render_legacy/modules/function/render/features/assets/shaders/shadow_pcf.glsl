#ifndef SHADOW_PCF_GLSL
#define SHADOW_PCF_GLSL
// =========================================================================
//  shadow_pcf.glsl — PCF (Percentage-Closer Filtering) shadow technique
//
//  This file implements the `directionalShadow / pointShadow / spotShadow`
//  contract used by lighting fragment shaders. It is one of multiple shadow
//  technique implementations selected at SPIR-V variant build time via the
//  `LUX_SHADOW_SAMPLER_HEADER` define (see deferred_lighting.frag header).
//
//  Provides: shadow atlas sampler (D32_SFLOAT, sampler2DArrayShadow),
//            light-space VP matrix SSBO, cascade selection, PCF + receiver-
//            plane depth bias sampling.
//
//  Usage: #include after lighting_common.glsl; do NOT include directly —
//         go through the `LUX_SHADOW_SAMPLER_HEADER` indirection.
//  Define LUX_LIGHT_SET before including to override the descriptor set
//  (default 3 for forward, use 2 for deferred).
// =========================================================================

#ifndef LUX_LIGHT_SET
#define LUX_LIGHT_SET 3
#endif

// ========== Shadow resources at bindings 4-6 ==========

/// Per-shadow-slice metadata (matches ShadowSliceGPU in ShadowMapTypes.hpp)
#include "shadow_common.glsl"

layout(set = LUX_LIGHT_SET, binding = 4, std430) readonly buffer ShadowSliceBuf {
    ShadowSliceGPU slices[];
} uShadowSlices;

/// Shadow atlas: 2D array texture with comparison sampler
layout(set = LUX_LIGHT_SET, binding = 5) uniform sampler2DArrayShadow uShadowAtlas;

/// Shadow configuration
layout(set = LUX_LIGHT_SET, binding = 6, std140) uniform ShadowConfigUBO {
    uint  total_slices;
    uint  dir_light_offset;
    uint  dir_cascade_count;
    uint  spot_light_offset;
    uint  spot_light_count;
    uint  point_light_offset;
    uint  point_light_count;
    float dir_split_is_normalized;
    float dir_split_near;
    float dir_split_far;
    uint  dir_caster_slot;   // slot of the directional whose cascades are at dir_light_offset (C-6)
    float scene_time;
} uShadowConfig;

layout(set = LUX_LIGHT_SET, binding = 7, std430) readonly buffer SpotShadowMapBuf {
    int spot_shadow_slice_index[];
} uSpotShadowMap;

layout(set = LUX_LIGHT_SET, binding = 8, std430) readonly buffer PointShadowMapBuf {
    int point_shadow_base_slice[];
} uPointShadowMap;

// ========== Cascade selection ==========

float directionalCascadeSplitValue(uint light_index, uint cascade_index)
{
    float raw = uDirectionalLights.lights[light_index].cascade_splits[cascade_index];
    if (uShadowConfig.dir_split_is_normalized > 0.5)
    {
        float t = clamp(raw, 0.0, 1.0);
        return mix(uShadowConfig.dir_split_near, uShadowConfig.dir_split_far, t);
    }
    return raw;
}

/// Select the cascade index for a directional light based on view-space depth.
uint selectCascade(uint light_index, float view_depth, uint cascade_count)
{
    for (uint i = 0u; i < cascade_count; ++i)
    {
        if (view_depth < directionalCascadeSplitValue(light_index, i))
            return i;
    }
    return cascade_count - 1u;
}

// ========== PCF Shadow Sampling ==========
//
// Receiver-plane depth bias is the canonical fix for PCF self-shadowing on
// tilted surfaces. The error comes from comparing each PCF tap's depth
// against the *centre* tap's reference, while the receiver's depth varies
// linearly across the kernel; that mismatch is what older shadow code papered
// over with a world-space `normal_bias` offset (which buys correctness on
// the lit side at the cost of Peter-Panning at contact). Here we compute
// ∂z/∂u and ∂z/∂v in shadow-space directly via screen-space derivatives —
// `dFdx`/`dFdy` of (uv, depth) — and correct each tap's reference depth
// before comparison. This is a mathematical identity, not a parameter.
//
// The 2×2 Jacobian J = [duvdx.xy, duvdy.xy] maps screen-space delta to
// shadow-UV delta; its inverse gives ∂z/∂uv from ∂z/∂xy = (duvdx.z, duvdy.z).
// At cascade boundaries adjacent screen pixels can be in different cascades,
// poisoning the gradient — we clamp the magnitude to a conservative cap
// (~one full-tile-of-depth per full-tile-of-uv = 1.0) so the worst-case
// boundary pixel sees at most "no further correction" rather than runaway.

/// Compute the receiver-plane depth gradient in shadow-tile-UV space.
/// Returns ∂z/∂u and ∂z/∂v.
///
/// Returns `vec2(0)` — i.e. falls back to no per-tap correction — at:
///   1. Screen-space geometry discontinuities. `dFdx`/`dFdy` of the shadow
///      position at a silhouette edge reflect the *jump* between two
///      different surfaces on adjacent screen pixels, not a slope; the
///      resulting "gradient" would point in a random direction and produce
///      a 1-2-pixel-wide dark strip along every screen-space silhouette of
///      a shadow receiver. Detected by checking the per-pixel change in
///      shadow-tile UV against a threshold expressed in tile-UV units —
///      a smooth slope keeps |dpdx.xy| well under ~1/100 of the tile, a
///      silhouette jump produces multi-tile values.
///   2. Cascade boundaries (adjacent pixels in different cascades — `det`
///      is near-zero or sign-changes; the conventional `abs(det) < eps`
///      catch handles this).
///   3. Degenerate gradients larger than 1 (one tile's depth per tile's UV)
///      — these never happen for real receivers; the clamp is a final
///      safety on top of the discontinuity guard.
vec2 computeReceiverPlaneDepthBias(vec3 shadow_pos_tile)
{
    vec3 dpdx = dFdx(shadow_pos_tile);
    vec3 dpdy = dFdy(shadow_pos_tile);

    // Discontinuity guard: any screen-pixel-to-pixel change in shadow-UV
    // larger than ~1% of the tile means adjacent screen pixels are on
    // different surfaces (a real continuous receiver — even a tilted one
    // at glancing angle — keeps this much smaller). Disable receiver-plane
    // correction at those pixels so the only bias still in effect is the
    // rasterizer's slope-scale + constant, which has no discontinuity
    // sensitivity.
    const float kMaxContinuousUVStep = 0.01;
    if (max(abs(dpdx.x), abs(dpdy.x)) > kMaxContinuousUVStep ||
        max(abs(dpdx.y), abs(dpdy.y)) > kMaxContinuousUVStep)
        return vec2(0.0);

    float det = dpdx.x * dpdy.y - dpdx.y * dpdy.x;
    if (abs(det) < 1e-8)
        return vec2(0.0);
    vec2 grad = vec2(
        dpdy.y * dpdx.z - dpdx.y * dpdy.z,
        dpdx.x * dpdy.z - dpdy.x * dpdx.z) / det;
    return clamp(grad, vec2(-1.0), vec2(1.0));
}

// ── Normal-offset bias ────────────────────────────────────────────────────
// Float (D32_SFLOAT) depth atlases make `vkCmdSetDepthBias`'s *constant* term
// almost inert: Vulkan derives the bias unit `r` from the primitive's maximum
// depth exponent, so at a receiver depth of ndc.z≈0.98 (typical for a floor a
// few units under a point light) `r` is ~1e-7 and a constant factor of a few
// hundred still produces a sub-texel offset. That leaves PERPENDICULAR
// receivers — the floor directly beneath a point light's -Y cube face, where
// the depth slope is ~0 so slope-scale bias also vanishes — with effectively
// no bias at all → shadow acne. The receiver-plane gradient can't save it
// either: it is ~0 at perpendicular and its discontinuity guard trips at the
// grazing side faces. Normal-offset bias is depth-format independent — nudge
// the receiver along its surface normal by a few shadow-texels' worth of world
// space before the depth compare. This is the canonical fix for the point-light
// acne stripes and the visible -Y-face square boundary on large flat floors.
const float kPcfNormalConstTexels = 1.5; // always-on offset → fixes perpendicular acne
const float kPcfNormalSlopeTexels = 2.0; // extra offset scaled by grazing (1 - N·L)

vec3 luxShadowSlicePosition(
    ShadowSliceGPU slice,
    vec3 view_relative_position)
{
    ViewGpuData view_data = uViews.views[uPC.view_index];
    return view_relative_position
         + vec3(view_data.camera_page.xyz - slice.origin_page.xyz)
             * slice.origin_local_page_size.w
         + view_data.camera_local_page_size.xyz
         - slice.origin_local_page_size.xyz;
}

/// Project a receiver into a slice's light-clip space twice: the raw geometric
/// projection (`geo_clip`, used for the frustum-coverage test) and the
/// normal-offset-biased projection (`biased_clip`, used for the depth reference
/// + sample UV).
///
/// Why two: the normal offset is purely a depth-bias trick. If the *biased*
/// position is used for the coverage test, a fragment that is geometrically
/// inside the frustum but whose offset nudged it just past the tile edge reads
/// as "outside" → returns fully-lit → bright speckles along the point-light
/// -Y cube-face square boundary (the 45° seam where -Y hands off to a side
/// face). Gating coverage on the un-offset `geo_clip` removes those speckles
/// while the biased projection still does its anti-acne job.
void projectShadowBiased(
    ShadowSliceGPU s, vec3 world_pos, vec3 world_normal, float normal_scale,
    out vec4 geo_clip, out vec4 biased_clip)
{
    // World-space size of one shadow texel at this receiver:
    //  • perspective (point/spot): the light frustum widens with distance, so
    //    world-texel = (NDC texel = 2·texel_size) · (light-axis distance = |w|).
    //    `light_vp·world_pos` yields that w directly (proj row 3 = (0,0,1,0)).
    //  • ortho (directional CSM): constant across the cascade — the CPU stores
    //    extent/resolution in `normal_bias`.
    vec3 slice_position = luxShadowSlicePosition(s, world_pos);
    geo_clip = s.light_vp * vec4(slice_position, 1.0);
    float world_texel = (s.depth_is_perspective != 0u)
        ? 2.0 * s.texel_size * max(abs(geo_clip.w), 1e-3)
        : s.normal_bias;
    float offset = world_texel *
        (kPcfNormalConstTexels + kPcfNormalSlopeTexels * clamp(normal_scale, 0.0, 1.0));
    vec3 biased = slice_position + normalize(world_normal) * offset;
    biased_clip = s.light_vp * vec4(biased, 1.0);
}

/// Sample shadow with a single comparison tap (no PCF).
float sampleShadow1Tap(uint slice_index, vec3 world_pos, vec3 world_normal, float normal_scale)
{
    ShadowSliceGPU s = uShadowSlices.slices[slice_index];
    vec4 geo_clip, light_clip;
    projectShadowBiased(s, world_pos, world_normal, normal_scale, geo_clip, light_clip);

    // Coverage test on the un-offset geometric projection (see
    // projectShadowBiased) so the normal offset can't speckle the seam.
    vec3 geo_ndc = geo_clip.xyz / geo_clip.w;
    vec2 geo_uv  = geo_ndc.xy * 0.5 + 0.5;
    if (geo_uv.x < 0.0 || geo_uv.x > 1.0 ||
        geo_uv.y < 0.0 || geo_uv.y > 1.0 ||
        geo_ndc.z > 1.0 || geo_ndc.z < 0.0)
        return 1.0;

    vec3 ndc = light_clip.xyz / light_clip.w;
    // Clamp the biased UV into the tile: at the seam the offset can nudge it a
    // fraction of a texel past [0,1]; the adjacent face already covers that, so
    // clamping to this tile's edge is the correct local occluder estimate.
    vec2 shadow_uv_local = clamp(ndc.xy * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    float current_depth = ndc.z;

    vec2 shadow_uv = s.atlas_uv_bias + shadow_uv_local * s.atlas_uv_scale;
    shadow_uv = clamp(shadow_uv, s.atlas_inner_uv_min, s.atlas_inner_uv_max);
    // No shader-side bias subtraction — rasterizer slope-scale + constant
    // bias have already shifted the recorded occluder depth.
    float ref_depth = clamp(current_depth, 0.0, 1.0);
    return texture(uShadowAtlas, vec4(shadow_uv, float(s.atlas_layer), ref_depth));
}

/// Sample shadow with 3×3 PCF (Percentage-Closer Filtering) with
/// per-tap receiver-plane depth bias correction.
float sampleShadowPCF(uint slice_index, vec3 world_pos, vec3 world_normal, float normal_scale)
{
    ShadowSliceGPU s = uShadowSlices.slices[slice_index];

    // Transform to light clip space with a normal-offset bias. The offset is the
    // primary self-shadow defence (see projectShadowBiased); the receiver-plane
    // gradient below then corrects each PCF tap for the receiver's local slope.
    vec4 geo_clip, light_clip;
    projectShadowBiased(s, world_pos, world_normal, normal_scale, geo_clip, light_clip);

    // Coverage test on the un-offset geometric projection: the normal offset is
    // a depth trick and must NOT make an in-frustum fragment read as "outside"
    // — that produced bright (fully-lit) speckles along the point-light -Y
    // cube-face square edge.
    vec3 geo_ndc = geo_clip.xyz / geo_clip.w;
    vec2 geo_uv  = geo_ndc.xy * 0.5 + 0.5;
    if (geo_uv.x < 0.0 || geo_uv.x > 1.0 ||
        geo_uv.y < 0.0 || geo_uv.y > 1.0 ||
        geo_ndc.z > 1.0 || geo_ndc.z < 0.0)
        return 1.0;

    vec3 ndc = light_clip.xyz / light_clip.w;

    // NDC [-1,1] -> local tile UV [0,1]. Clamp the biased UV into the tile: at
    // the cube-face seam the offset can nudge it a fraction of a texel past
    // [0,1]; the neighbouring face covers that region, so clamping to this
    // tile's edge is the correct local occluder estimate (and stops speckles).
    vec2 shadow_uv_local = clamp(ndc.xy * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    float current_depth = ndc.z;

    // Receiver-plane depth gradient in tile-UV space (∂z/∂u, ∂z/∂v).
    // dFdx/dFdy operate on the freshly-derived shadow position, so the
    // gradient reflects how the receiver's depth changes per shadow-UV unit.
    vec2 rpdb = computeReceiverPlaneDepthBias(
        vec3(shadow_uv_local, current_depth));

    // Map local tile UV into atlas page UV.
    vec2 shadow_uv = s.atlas_uv_bias + shadow_uv_local * s.atlas_uv_scale;
    vec2 tile_inner_min = s.atlas_inner_uv_min;
    vec2 tile_inner_max = s.atlas_inner_uv_max;

    // No shader-side depth subtraction: the rasterizer has already biased
    // the recorded occluder depth via `vkCmdSetDepthBias` (slope-scale +
    // constant), and the receiver-plane gradient below corrects each
    // PCF tap for the receiver's local slope. Subtracting an additional
    // constant from `current_depth` here would double-bias.
    float base_depth = current_depth;

    // 3×3 PCF kernel with per-tap receiver-plane correction.
    float shadow = 0.0;
    float texel = s.texel_size;
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            vec2 tile_offset = vec2(float(x), float(y)) * texel;     // in tile UV
            vec2 atlas_offset = tile_offset * s.atlas_uv_scale;       // in atlas UV
            vec2 sample_uv = shadow_uv + atlas_offset;

            // Clamp taps that fall outside the tile interior to the tile
            // edge.  Previously these were counted as 1.0 (fully lit), which
            // caused bright seam lines along point-light cube-face boundaries.
            sample_uv = clamp(sample_uv, tile_inner_min, tile_inner_max);

            // Per-tap depth correction: where the receiver's plane sits at
            // this offset, not at the centre. This is the formal fix for
            // the "PCF on tilted surface" failure mode that the deleted
            // world-space normal_bias was a workaround for.
            float tap_depth = base_depth + dot(rpdb, tile_offset);
            tap_depth = clamp(tap_depth, 0.0, 1.0);

            shadow += texture(uShadowAtlas,
                              vec4(sample_uv, float(s.atlas_layer), tap_depth));
        }
    }
    return shadow / 9.0;
}

bool directionalCascadeCovered(
    uint slice_index, vec3 world_pos, vec3 world_normal, float normal_scale)
{
    ShadowSliceGPU s = uShadowSlices.slices[slice_index];
    vec4 light_clip = s.light_vp * vec4(luxShadowSlicePosition(s, world_pos), 1.0);
    vec3 ndc = light_clip.xyz / light_clip.w;
    vec2 shadow_uv = ndc.xy * 0.5 + 0.5;

    return !(shadow_uv.x < 0.0 || shadow_uv.x > 1.0 ||
             shadow_uv.y < 0.0 || shadow_uv.y > 1.0 ||
             ndc.z > 1.0 || ndc.z < 0.0);
}

float sampleDirectionalCascadeWithFallback(
    uint start_cascade,
    uint offset,
    uint cascade_count,
    vec3 world_pos,
    vec3 world_normal,
    float normal_scale,
    out uint used_cascade)
{
    // Try selected cascade first, then larger cascades, then smaller cascades.
    for (uint cascade = start_cascade; cascade < cascade_count; ++cascade)
    {
        uint si = offset + cascade;
        if (!directionalCascadeCovered(si, world_pos, world_normal, normal_scale))
            continue;

        used_cascade = cascade;
        return sampleShadowPCF(si, world_pos, world_normal, normal_scale);
    }

    for (uint cascade = start_cascade; cascade > 0u; --cascade)
    {
        uint prev = cascade - 1u;
        uint si = offset + prev;
        if (!directionalCascadeCovered(si, world_pos, world_normal, normal_scale))
            continue;

        used_cascade = prev;
        return sampleShadowPCF(si, world_pos, world_normal, normal_scale);
    }

    used_cascade = cascade_count;
    return 1.0;
}

/// Compute shadow factor for a directional light using CSM.
float directionalShadow(uint light_index, vec3 world_pos, vec3 world_normal,
                         float view_depth)
{
    if ((uDirectionalLights.lights[light_index].flags & 1u) == 0u)
        return 1.0;
    if (light_index != uShadowConfig.dir_caster_slot)
        return 1.0; // cascades are allocated for the single CPU-chosen caster slot (C-6)

    uint offset = uShadowConfig.dir_light_offset;
    uint configured_count = uShadowConfig.dir_cascade_count;
    uint declared_count = uDirectionalLights.lights[light_index].cascade_count;
    uint cascade_count = min(configured_count, declared_count);
    if (cascade_count == 0u) return 1.0;

    vec3 L = normalize(-uDirectionalLights.lights[light_index].direction);
    float ndotl = max(dot(normalize(world_normal), L), 0.0);
    float normal_scale = max(0.15, 1.0 - ndotl);

    // Single-slice directional shadow mode: bypass cascade selection/blending.
    if (cascade_count <= 1u)
    {
        uint si = offset;
        if (si >= uShadowConfig.total_slices)
            return 1.0;
        return sampleShadowPCF(si, world_pos, world_normal, normal_scale);
    }

    uint start_cascade = selectCascade(light_index, view_depth, cascade_count);

    uint used_primary = start_cascade;
    float primary = sampleDirectionalCascadeWithFallback(
        start_cascade, offset, cascade_count, world_pos, world_normal, normal_scale, used_primary);
    if (used_primary != start_cascade || start_cascade + 1u >= cascade_count)
        return primary;

    // Blend zone near the end of each cascade to remove hard cascade seams.
    float split_far = directionalCascadeSplitValue(light_index, start_cascade);
    float split_near = (start_cascade > 0u)
        ? directionalCascadeSplitValue(light_index, start_cascade - 1u)
        : 0.0;

    float split_span = max(split_far - split_near, 1e-4);
    float blend_zone = split_span * 0.10; // 10% tail blending
    float blend_start = split_far - blend_zone;
    if (view_depth <= blend_start)
        return primary;

    uint used_secondary = start_cascade + 1u;
    float secondary = sampleDirectionalCascadeWithFallback(
        start_cascade + 1u, offset, cascade_count, world_pos, world_normal, normal_scale, used_secondary);
    float blend_t = clamp((view_depth - blend_start) / max(split_far - blend_start, 1e-4), 0.0, 1.0);
    return mix(primary, secondary, blend_t);
}

/// Compute shadow factor for a spot light.
float spotShadow(uint light_index, vec3 world_pos, vec3 world_normal)
{
    if (light_index >= uint(uSpotShadowMap.spot_shadow_slice_index.length()))
        return 1.0;
    int mapped = uSpotShadowMap.spot_shadow_slice_index[light_index];
    if (mapped < 0)
        return 1.0;
    uint slice_index = uint(mapped);
    if (slice_index >= uShadowConfig.total_slices)
        return 1.0;

    vec3 L = normalize(luxSpotLightViewPosition(light_index) - world_pos);
    float ndotl = max(dot(normalize(world_normal), L), 0.0);
    // Keep spot-light contact shadows tighter than directional cascades.
    float normal_scale = max(0.02, 1.0 - ndotl);
    return sampleShadowPCF(slice_index, world_pos, world_normal, normal_scale);
}

/// Select cube face index (0-5) from a direction vector.
/// Face order: +X, -X, +Y, -Y, +Z, -Z (matches CPU buildSlicesForFrame).
uint selectCubeFace(vec3 dir)
{
    vec3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z)
        return dir.x > 0.0 ? 0u : 1u;
    else if (a.y >= a.x && a.y >= a.z)
        return dir.y > 0.0 ? 2u : 3u;
    else
        return dir.z > 0.0 ? 4u : 5u;
}

/// Compute shadow factor for a point light using 6 cube-face shadow slices.
/// Uses distance-adaptive PCF: 1-tap for distant fragments, 3×3 for close ones.
float pointShadow(uint light_index, vec3 world_pos, vec3 world_normal)
{
    if (light_index >= uint(uPointShadowMap.point_shadow_base_slice.length()))
        return 1.0;
    int mapped = uPointShadowMap.point_shadow_base_slice[light_index];
    if (mapped < 0)
        return 1.0;
    uint base = uint(mapped);
    if (base + 5u >= uShadowConfig.total_slices)
        return 1.0;

    vec3 lv = world_pos - luxPointLightViewPosition(light_index);
    uint face = selectCubeFace(lv);
    uint si   = base + face;

    vec3 L = normalize(luxPointLightViewPosition(light_index) - world_pos);
    float ndotl = max(dot(normalize(world_normal), L), 0.0);
    // Keep point-light contact shadows tighter to preserve blocky silhouettes.
    float normal_scale = max(0.02, 1.0 - ndotl);

    // Distance-adaptive PCF: use cheap 1-tap beyond 60% of light range
    float d = length(lv);
    float range = uPointLights.lights[light_index].range;
    if (d > range * 0.6)
        return sampleShadow1Tap(si, world_pos, world_normal, normal_scale);

    return sampleShadowPCF(si, world_pos, world_normal, normal_scale);
}

/// Compute view-space depth for cascade selection.
/// Requires lighting_common.glsl (uViews, uPC) to be included first.
float computeViewDepth(vec3 world_pos)
{
    ViewGpuData view_data = uViews.views[uPC.view_index];
    return -(view_data.view * vec4(world_pos, 1.0)).z;
}

#endif // SHADOW_PCF_GLSL
