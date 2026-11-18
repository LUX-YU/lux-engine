#pragma once

#include <array>
#include <cstdint>
#include <lux/engine/render/capabilities/ClusterGrid.hpp>
#include <string_view>

namespace lux::render
{
    struct ClusterBoundsGpu final
    {
        std::array<float, 4> minimum{}, maximum{};
    };

    struct ClusterGridGpu final
    {
        std::array<std::uint32_t, 4> cells{};  // xyz dimensions, w EClusterDepth.
        std::array<std::uint32_t, 4> limits{}; // cluster count, index capacity, reserved.
        std::array<float, 4> position_near{}, direction_far{};
        std::array<float, 16> world_to_clip{};
    };

    struct ClusterStatusGpu final
    {
        std::uint32_t required_indices = 0; // Saturates at UINT32_MAX; never wraps.
        std::uint32_t flags = 0;            // 1: capacity, 2: invalid projection, 4: saturated sum.
        std::uint32_t reserved[2]{};
    };

    static_assert(sizeof(ClusterBoundsGpu) == 32 && sizeof(ClusterGridGpu) == 128);
    static_assert(sizeof(ClusterStatusGpu) == 16);
    inline constexpr std::string_view kClusterGrid = "lux.light.cluster.grid.forward-z.v1";
    inline constexpr std::string_view kClusterOffsets = "lux.light.cluster.exclusive-offsets.v1";
    // Legacy packed index: low 31 bits dense index, bit 31 selects spot instead of point.
    inline constexpr std::string_view kClusterIndices = "lux.light.cluster.point-spot-indices.v1";
    inline constexpr std::string_view kClusterStatus = "lux.light.cluster.status.v1";
} // namespace lux::render
