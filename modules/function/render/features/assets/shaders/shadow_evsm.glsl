#ifndef SHADOW_EVSM_GLSL
#define SHADOW_EVSM_GLSL
// =========================================================================
//  shadow_evsm.glsl — EVSM (Exponential Variance Shadow Maps) technique
//
//  Implements `directionalShadow / pointShadow / spotShadow` against the
//  pre-filtered RGBA16F atlas produced by `shadow_evsm_caster.frag` +
//  `shadow_evsm_blur_{h,v}.comp`. Selected at SPIR-V variant build time via
//  `LUX_SHADOW_TECHNIQUE_ID == 1` (see deferred_lighting.frag header).
//
//  Math (.internal/plan/evsm-shadow-implementation-guide.md §2.4):
//    - Receiver depth z is warped through dual exponential:
//        wpos = exp(c+ · z),   wneg = -exp(-c- · z)
//    - Atlas stores E[wpos], E[wpos²], E[wneg], E[wneg²] (per-channel after
//      Gaussian blur). Chebyshev's one-sided inequality bounds the occlusion
//      probability:
//        p_max(d) = σ² / (σ² + (d - μ)²)     for d > μ
//      where μ, σ² are the per-channel filtered mean and variance.
//    - Dual-ESM (positive + negative): min of the two Chebyshev bounds.
//      Tighter than VSM, no per-light bias tuning required.
//    - Bleed reduction: clamps low-probability tails to fight light bleeding
//      where two distinct receivers share an atlas footprint.
//
//  Bindings 4 / 6 / 7 / 8 / config UBO are shared with PCF — the variant
//  baker ensures only one of shadow_pcf.glsl / shadow_evsm.glsl is included
//  per SPIR-V, so re-declaring them at the same slot is benign. EVSM-only
//  bindings 9 (atlas) and 10 (config) coexist with PCF's binding 5 in the
//  descriptor set layout; in PCF SPIR-V they are unused and the host can
//  bind dummy resources (C3c host-side wiring).
// =========================================================================

#ifndef LUX_LIGHT_SET
#define LUX_LIGHT_SET 3
#endif

// ── Shared slice / config / per-light-type slot maps ──────────────────────

#include "shadow_common.glsl"

layout(set = LUX_LIGHT_SET, binding = 4, std430) readonly buffer ShadowSliceBuf {
    ShadowSliceGPU slices[];
} uShadowSlices;

// Binding 5 in PCF is `sampler2DArrayShadow uShadowAtlas` — skipped here,
// EVSM uses binding 9 (regular sampler) for its RGBA16F blurred atlas.

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

// ── EVSM-only bindings: blurred moment atlas + per-frame technique config.
//    Host (C3c) writes a dummy at slot 9/10 when PCF is active so the
//    layout stays valid for both variants.

layout(set = LUX_LIGHT_SET, binding = 9) uniform sampler2DArray uShadowAtlasEVSM;

layout(set = LUX_LIGHT_SET, binding = 10, std140) uniform EvsmConfigUBO {
    float pos_exponent;    // must match shadow_evsm_caster.frag push constant
    float neg_exponent;    // must match shadow_evsm_caster.frag push constant
    float bleed_reduction; // 0.2 default — VSM light-leak clamp
    float _pad;
} uEvsmConfig;

// ── Shared cascade / cube selection (technique-agnostic) ──────────────────

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

uint selectCascade(uint light_index, float view_depth, uint cascade_count)
{
    for (uint i = 0u; i < cascade_count; ++i)
    {
        if (view_depth < directionalCascadeSplitValue(light_index, i))
            return i;
    }
    return cascade_count - 1u;
}

uint selectCubeFace(vec3 dir)
{
    vec3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z) return dir.x > 0.0 ? 0u : 1u;
    if (a.y >= a.x && a.y >= a.z) return dir.y > 0.0 ? 2u : 3u;
    return dir.z > 0.0 ? 4u : 5u;
}

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

float computeViewDepth(vec3 world_pos)
{
    ViewGpuData view_data = uViews.views[uPC.view_index];
    return -(view_data.view * vec4(world_pos, 1.0)).z;
}

// ── EVSM core sampling ────────────────────────────────────────────────────

/// Dual exponential warp of a normalized depth value [0,1] → matches the
/// caster's per-fragment write.
///
/// The exponents MUST match `shadow_evsm_caster.frag`'s kEVSMPosExponent /
/// kEVSMNegExponent (both hardcoded to 5.0 — RGBA16F-safe; see that shader's
/// header for the overflow budget). uEvsmConfig.{pos,neg}_exponent is seeded
/// to the same 5.0 host-side; we keep the sampler warp pinned to the caster's
/// literal so a future config tweak can't silently desync the two halves of
/// the moment pair while the caster stays hardcoded.
vec2 warpDepth(float depth)
{
    const float kPosExp = 5.0;
    const float kNegExp = 5.0;
    return vec2( exp( kPosExp * depth),
                -exp(-kNegExp * depth));
}

/// Chebyshev's one-sided inequality on the first two moments.
/// Returns the maximum occlusion probability bound in [0,1] — 1.0 is fully
/// lit, 0.0 is fully shadowed.
float chebyshev(vec2 moments, float receiver, float min_variance)
{
    // Receiver closer than (or equal to) the filtered mean → unoccluded.
    if (receiver <= moments.x)
        return 1.0;
    float variance = max(moments.y - moments.x * moments.x, min_variance);
    float d        = receiver - moments.x;
    return variance / (variance + d * d);
}

/// EVSM tap: project, fetch moments, run dual Chebyshev, apply bleed clamp.
/// Returns 1.0 if outside the tile (lit fallback).
float sampleEVSMOne(uint slice_index, vec3 world_pos)
{
    ShadowSliceGPU s = uShadowSlices.slices[slice_index];
    vec4 light_clip  = s.light_vp * vec4(luxShadowSlicePosition(s, world_pos), 1.0);
    vec3 ndc         = light_clip.xyz / light_clip.w;

    vec2 local_uv = ndc.xy * 0.5 + 0.5;
    float depth   = ndc.z;
    if (local_uv.x < 0.0 || local_uv.x > 1.0 ||
        local_uv.y < 0.0 || local_uv.y > 1.0 ||
        depth     > 1.0 || depth     < 0.0)
        return 1.0;

    // Perspective slices: recover linear [0,1] depth from hyperbolic NDC z.
    // MUST match shadow_evsm_caster.frag's linearization exactly, otherwise the
    // receiver depth and the stored moments live on different curves and every
    // sample reads as occluded. Ortho (directional) slices skip this.
    if (s.depth_is_perspective != 0u)
    {
        float vz = (s.shadow_far * s.shadow_near)
                 / (s.shadow_far - depth * (s.shadow_far - s.shadow_near));
        depth = clamp((vz - s.shadow_near) / max(s.shadow_far - s.shadow_near, 1e-6), 0.0, 1.0);
    }

    vec2 atlas_uv = s.atlas_uv_bias + local_uv * s.atlas_uv_scale;
    atlas_uv      = clamp(atlas_uv, s.atlas_inner_uv_min, s.atlas_inner_uv_max);

    // Hardware-filtered RGBA16F fetch — bilinear over pre-filtered moments
    // is exactly what makes EVSM bias-free.
    vec4 moments = texture(uShadowAtlasEVSM,
                           vec3(atlas_uv, float(s.atlas_layer)));

    // Cleared-region guard. The moment atlas is LOAD_OP_CLEAR'd to color
    // (0,0,0,1) each frame (RGVulkanRecorder's hardcoded color clear), but the
    // caster ALWAYS writes moments.x = exp(c+ · depth) >= 1 (depth in [0,1] =>
    // pos in [1, e^5]). So moments.x < 1 means this texel was NEVER touched by a
    // caster — it is a cleared, occluder-free region (e.g. a receiver projecting
    // into the tile area outside the caster geometry). The Chebyshev test on the
    // invalid (0,…) moments otherwise collapses to ~0 (fully shadowed); as casters
    // / cascades repack across the atlas the spurious dark patches flicker
    // frame-to-frame. Treat sub-caster magnitude as fully lit.
    if (moments.x < 1.0)
        return 1.0;

    // Min-variance acts as a noise floor for the Chebyshev bound. The
    // canonical Frostbite value is 2e-5 — fine for RGBA32F + Frostbite-grade
    // exponents (pos=40 / neg=5) where moments.x precision is sub-ULP. With
    // RGBA16F + capped exponents (pos=neg=5), fp16 stores moments.x at
    // ~135 with precision ~0.13 LSB. Receiver depth and atlas depth come
    // from independent rasterization paths so they differ by ≤2 ULP =
    // ~0.25 even on truly co-planar surfaces. Squared that's d²≈0.06 —
    // overwhelms a 2e-5 variance floor and the Chebyshev bound collapses
    // to ≈0 on every lit fragment, giving fully-shadowed false positives.
    //
    // We use per-channel min-variance scaled to the moment magnitude — for
    // pos channel that's effectively (mean·1%)² which absorbs ≥1% rounding
    // noise; for neg channel the values are tiny (~1e-2 max) so the same
    // ratio scales appropriately.
    vec2 warped_recv = warpDepth(depth);
    const float kRelativeMinVariance = 0.01;
    float min_var_pos = max(moments.x * moments.x * kRelativeMinVariance * kRelativeMinVariance, 1e-6);
    float min_var_neg = max(moments.z * moments.z * kRelativeMinVariance * kRelativeMinVariance, 1e-6);
    float p_pos = chebyshev(moments.xy, warped_recv.x, min_var_pos);
    float p_neg = chebyshev(moments.zw, warped_recv.y, min_var_neg);
    float p     = min(p_pos, p_neg);

    // Light-bleed reduction: remap [br,1] → [0,1], clamping the low-probability
    // tail to 0. `br` is driven by the host config UBO (seeded to 0.2). It must
    // stay small: the term is a 1/(1-br) amplifier, so an over-large br (the
    // old 0.9 debug value = 10× gain) blows fp16 Chebyshev noise on *lit*
    // surfaces up into visible partial shadow — the cascade/light-frustum
    // rectangles of mottled grey that looked like "non-uniform" shadows.
    float br = clamp(uEvsmConfig.bleed_reduction, 0.0, 0.6);
    return clamp((p - br) / max(1.0 - br, 1e-3), 0.0, 1.0);
}

// ── Cascade fallback (mirrors PCF for cross-cascade seam smoothing) ───────

bool directionalCascadeCovered_EVSM(uint slice_index, vec3 world_pos)
{
    ShadowSliceGPU s = uShadowSlices.slices[slice_index];
    vec4 lc          = s.light_vp * vec4(luxShadowSlicePosition(s, world_pos), 1.0);
    vec3 ndc         = lc.xyz / lc.w;
    vec2 uv          = ndc.xy * 0.5 + 0.5;
    return !(uv.x < 0.0 || uv.x > 1.0 ||
             uv.y < 0.0 || uv.y > 1.0 ||
             ndc.z > 1.0 || ndc.z < 0.0);
}

float sampleDirectionalCascadeWithFallback_EVSM(
    uint start_cascade, uint offset, uint cascade_count,
    vec3 world_pos, out uint used_cascade)
{
    for (uint c = start_cascade; c < cascade_count; ++c)
    {
        uint si = offset + c;
        if (!directionalCascadeCovered_EVSM(si, world_pos))
            continue;
        used_cascade = c;
        return sampleEVSMOne(si, world_pos);
    }
    for (uint c = start_cascade; c > 0u; --c)
    {
        uint prev = c - 1u;
        uint si   = offset + prev;
        if (!directionalCascadeCovered_EVSM(si, world_pos))
            continue;
        used_cascade = prev;
        return sampleEVSMOne(si, world_pos);
    }
    used_cascade = cascade_count;
    return 1.0;
}

// ── Public API (matches the shadow_pcf.glsl contract) ─────────────────────

float directionalShadow(uint light_index, vec3 world_pos, vec3 world_normal,
                        float view_depth)
{
    if ((uDirectionalLights.lights[light_index].flags & 1u) == 0u) return 1.0;
    if (light_index != uShadowConfig.dir_caster_slot) return 1.0;   // cascades belong to the CPU-chosen caster slot (C-6)

    uint offset         = uShadowConfig.dir_light_offset;
    uint configured_n   = uShadowConfig.dir_cascade_count;
    uint declared_n     = uDirectionalLights.lights[light_index].cascade_count;
    uint cascade_count  = min(configured_n, declared_n);
    if (cascade_count == 0u) return 1.0;

    if (cascade_count <= 1u)
    {
        uint si = offset;
        if (si >= uShadowConfig.total_slices) return 1.0;
        return sampleEVSMOne(si, world_pos);
    }

    uint start = selectCascade(light_index, view_depth, cascade_count);

    uint used_primary = start;
    float primary = sampleDirectionalCascadeWithFallback_EVSM(
        start, offset, cascade_count, world_pos, used_primary);
    if (used_primary != start || start + 1u >= cascade_count)
        return primary;

    // Cross-cascade tail-blend — same 10% zone as PCF.
    float split_far  = directionalCascadeSplitValue(light_index, start);
    float split_near = (start > 0u)
        ? directionalCascadeSplitValue(light_index, start - 1u) : 0.0;
    float split_span = max(split_far - split_near, 1e-4);
    float blend_zone = split_span * 0.10;
    float blend_start = split_far - blend_zone;
    if (view_depth <= blend_start) return primary;

    uint used_secondary = start + 1u;
    float secondary = sampleDirectionalCascadeWithFallback_EVSM(
        start + 1u, offset, cascade_count, world_pos, used_secondary);
    float t = clamp((view_depth - blend_start) / max(split_far - blend_start, 1e-4),
                    0.0, 1.0);
    return mix(primary, secondary, t);
}

float spotShadow(uint light_index, vec3 world_pos, vec3 world_normal)
{
    if (light_index >= uint(uSpotShadowMap.spot_shadow_slice_index.length()))
        return 1.0;
    int mapped = uSpotShadowMap.spot_shadow_slice_index[light_index];
    if (mapped < 0) return 1.0;
    uint si = uint(mapped);
    if (si >= uShadowConfig.total_slices) return 1.0;
    return sampleEVSMOne(si, world_pos);
}

float pointShadow(uint light_index, vec3 world_pos, vec3 world_normal)
{
    if (light_index >= uint(uPointShadowMap.point_shadow_base_slice.length()))
        return 1.0;
    int mapped = uPointShadowMap.point_shadow_base_slice[light_index];
    if (mapped < 0) return 1.0;
    uint base = uint(mapped);
    if (base + 5u >= uShadowConfig.total_slices) return 1.0;

    vec3 lv   = world_pos - luxPointLightViewPosition(light_index);
    uint face = selectCubeFace(lv);
    return sampleEVSMOne(base + face, world_pos);
}

#endif // SHADOW_EVSM_GLSL
