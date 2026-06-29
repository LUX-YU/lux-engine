#pragma once
/**
 * @file PickResult.hpp
 * @brief Result payload for GPU pick / hit-test queries.
 */

#include <lux/engine/render/core/FeatureHandle.hpp>

#include <cstdint>
#include <limits>

namespace lux::render
{

/// GPU pick query result.
struct PickResult
{
    /// The feature whose geometry was hit (invalid if no hit).
    FeatureHandle feature{};

    /// Instance index within the feature (e.g., instanced mesh index).
    uint32_t instance_index{ (std::numeric_limits<uint32_t>::max)() };

    /// Screen-space query coordinates that produced this result.
    float query_x{ 0.0f };
    float query_y{ 0.0f };

    /// Normalized depth at the hit point (0 = near, 1 = far).
    float depth{ 1.0f };

    /// Whether the ray actually hit geometry.
    bool hit{ false };

    [[nodiscard]] explicit operator bool() const noexcept { return hit; }
};

} // namespace lux::render
