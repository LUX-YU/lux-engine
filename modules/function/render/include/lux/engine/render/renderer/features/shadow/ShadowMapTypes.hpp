#pragma once
/**
 * @file ShadowMapTypes.hpp
 * @brief Shadow-related types shared between ShadowMapFeature and ShadowResources.
 */

#include <Eigen/Core>
#include <cstdint>
#include <array>

namespace lux::render
{
    /// GPU-side per-shadow-slice metadata (matches ShadowSliceGPU in shadow_common.glsl).
    struct alignas(16) ShadowSliceGPU
    {
        Eigen::Matrix4f light_vp;     // Light-space view-projection matrix
        float           bias;         // Depth comparison bias
        float           slope_bias;   // Raster slope depth bias
        float           normal_bias;  // Normal offset bias
        float           texel_size;   // 1.0 / tile shadow resolution
        Eigen::Vector2f atlas_uv_scale; // Tile scale in page UV [0,1]
        Eigen::Vector2f atlas_uv_bias;  // Tile origin in page UV [0,1]
        Eigen::Vector2f atlas_inner_uv_min; // Inner sampling rect min UV (atlas space)
        Eigen::Vector2f atlas_inner_uv_max; // Inner sampling rect max UV (atlas space)
        uint32_t        atlas_layer;  // Layer index in the shadow map 2D array
        // EVSM depth-linearization params (reuse old padding — no size change).
        // Perspective (point/spot) shadows must warp a LINEAR depth metric, not
        // the hyperbolic post-projection NDC z, or fp16 moment precision
        // collapses at distance (catastrophic cancellation in the variance).
        // Directional/ortho slices leave depth_is_perspective = 0 and keep using
        // NDC z directly (already linear). near/far are the light projection's
        // clip planes used to recover view-space distance from NDC z.
        float           shadow_near;          // Light projection near plane (perspective only)
        float           shadow_far;           // Light projection far plane (perspective only)
        uint32_t        depth_is_perspective; // 1 = perspective slice (linearize), 0 = ortho
    };
    static_assert(sizeof(ShadowSliceGPU) % 16 == 0);
    static_assert(sizeof(ShadowSliceGPU) == 128, "ShadowSliceGPU must stay 128 B (std430 parity)");

    /// GPU-side shadow configuration (matches ShadowConfigGPU in shadow_common.glsl).
    struct alignas(16) ShadowConfigGPU
    {
        uint32_t total_slices;       // Total active shadow slices
        uint32_t dir_light_offset;   // Slice offset for directional lights
        uint32_t dir_cascade_count;  // Cascades for main directional light
        uint32_t spot_light_offset;  // Slice offset for spot lights
        uint32_t spot_light_count;   // Number of spot light shadow slices
        uint32_t point_light_offset; // Slice offset for point lights
        uint32_t point_light_count;  // Number of point light shadow slices
        float    dir_split_is_normalized; // 1.0: cascade_splits are normalized [0,1], 0.0: absolute view depth
        float    dir_split_near;          // Camera near for normalized directional splits
        float    dir_split_far;           // Camera far for normalized directional splits
        // Slot of the directional light whose cascades occupy dir_light_offset.
        // The CPU picks the first shadow-CASTING directional at ANY slot, so the
        // shader must compare light_index against this rather than hardcoding 0
        // (else a caster at slot 1+ uploads slices the shader never samples). (C-6)
        uint32_t dir_caster_slot;
    };
    static_assert(sizeof(ShadowConfigGPU) % 16 == 0);

    /// Default maximum number of shadow slices (cascades + spot + point).
    inline constexpr uint32_t kDefaultMaxShadowSlices = 128;
    inline constexpr uint32_t kMaxShadowSlices = kDefaultMaxShadowSlices;

    /// Default shadow atlas page resolution.
    inline constexpr uint32_t kDefaultShadowAtlasPageResolution = 4096;

    /// Default number of atlas pages (array layers).
    inline constexpr uint32_t kDefaultShadowAtlasPageCount = 4;

} // namespace lux::render
