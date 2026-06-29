#pragma once
/**
 * @file ShadowSliceMath.hpp
 * @brief Pure (GPU-free) shadow-slice projection helpers, split out of
 *        ShadowMapFeature::buildSlicesForView so the math is unit-testable.
 *
 * Everything here operates on plain POD inputs (positions, directions, clip
 * planes, an atlas tile) and produces a POD ShadowSliceGPU — no Vulkan, no
 * device (see shadow_slice_math_test). The atlas packing, light selection, and
 * cascade-budget orchestration stay in ShadowMapFeature.cpp.
 */

#include <lux/engine/render/renderer/features/shadow/ShadowMapTypes.hpp> // ShadowSliceGPU (+ Eigen/Core)

#include <Eigen/Geometry> // Eigen::Vector3f::cross

#include <algorithm>
#include <cstdint>

namespace lux::render
{
    /// One packed shadow tile in the atlas (page layer + UV rect + resolution).
    struct ShadowTileAllocation
    {
        uint32_t        layer{0};
        uint32_t        x{0};
        uint32_t        y{0};
        uint32_t        resolution{1};
        Eigen::Vector2f uv_scale{1.0f, 1.0f};
        Eigen::Vector2f uv_bias{0.0f, 0.0f};
    };

    /// Fill a slice's atlas-sampling metadata (texel size, UV scale/bias, the
    /// one-texel-inset inner sampling rect, and the array layer) from a tile.
    inline void assignAtlasTileMetadata(ShadowSliceGPU& slice, const ShadowTileAllocation& tile)
    {
        const float resolution = static_cast<float>(std::max(tile.resolution, 1u));

        slice.texel_size     = 1.0f / resolution;
        slice.atlas_uv_scale = tile.uv_scale;
        slice.atlas_uv_bias  = tile.uv_bias;

        // Keep a one-texel inset so +/-1 PCF taps stay inside the tile interior.
        const Eigen::Vector2f guard = tile.uv_scale * slice.texel_size;
        Eigen::Vector2f inner_min = tile.uv_bias + guard;
        Eigen::Vector2f inner_max = tile.uv_bias + tile.uv_scale - guard;
        const Eigen::Vector2f center = tile.uv_bias + tile.uv_scale * 0.5f;

        if (inner_min.x() > inner_max.x())
        {
            inner_min.x() = center.x();
            inner_max.x() = center.x();
        }
        if (inner_min.y() > inner_max.y())
        {
            inner_min.y() = center.y();
            inner_max.y() = center.y();
        }

        slice.atlas_inner_uv_min = inner_min;
        slice.atlas_inner_uv_max = inner_max;
        slice.atlas_layer        = tile.layer;
    }

    /// Build a perspective shadow slice — a spot light, or one point-light cube
    /// face. This is the construction shared by both paths (review M25).
    ///
    /// @param pos            Light world position (projection center).
    /// @param fwd            Unit forward (look) direction.
    /// @param up_seed        Seed up vector; re-orthonormalized against @p fwd.
    /// @param near_z,far_z   Light projection clip planes.
    /// @param proj_xy_scale  Reciprocal half-FOV tangent: 1/tan(fov/2) for a spot,
    ///                       1/overlap for a (slightly widened 90°) cube face.
    /// @param shadow_bias    The light's raw shadow bias (scaled into bias/slope_bias).
    /// @param tile           The packed atlas tile this slice renders into.
    inline ShadowSliceGPU makePerspectiveLightSlice(
        const Eigen::Vector3f&      pos,
        const Eigen::Vector3f&      fwd,
        const Eigen::Vector3f&      up_seed,
        float                       near_z,
        float                       far_z,
        float                       proj_xy_scale,
        float                       shadow_bias,
        const ShadowTileAllocation& tile)
    {
        Eigen::Matrix4f proj = Eigen::Matrix4f::Zero();
        proj(0, 0) = proj_xy_scale;
        proj(1, 1) = -proj_xy_scale;
        proj(2, 2) = far_z / (far_z - near_z);
        proj(2, 3) = -(far_z * near_z) / (far_z - near_z);
        proj(3, 2) = 1.f;

        const Eigen::Vector3f right = fwd.cross(up_seed).normalized();
        const Eigen::Vector3f up    = right.cross(fwd).normalized();

        Eigen::Matrix4f view_m = Eigen::Matrix4f::Identity();
        view_m.block<1, 3>(0, 0) = right.transpose();
        view_m.block<1, 3>(1, 0) = up.transpose();
        view_m.block<1, 3>(2, 0) = fwd.transpose();
        view_m(0, 3) = -right.dot(pos);
        view_m(1, 3) = -up.dot(pos);
        view_m(2, 3) = -fwd.dot(pos);

        ShadowSliceGPU slice{};
        slice.light_vp = proj * view_m;
        // Point / spot lights use 4x the constant bias scale of directional — a
        // receiver perpendicular to the light (e.g. the floor directly under a
        // downward-facing light) collapses slope_bias to ~0, leaving only the
        // constant term, so they need the extra headroom to avoid acne.
        slice.bias        = shadow_bias * 1024.0f;
        slice.slope_bias  = std::max(shadow_bias * 800.0f, 1e-5f);
        slice.normal_bias = 0.0f; // legacy field; no longer read by the shader
        // Perspective slice: EVSM must linearize the hyperbolic NDC z via these
        // clip planes before warping (see ShadowSliceGPU comment).
        slice.shadow_near = near_z;
        slice.shadow_far  = far_z;
        slice.depth_is_perspective = 1u;
        assignAtlasTileMetadata(slice, tile);
        return slice;
    }

} // namespace lux::render
