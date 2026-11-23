#pragma once

#include <array>
#include <cstdint>

namespace lux::render
{
    struct ClusterBoundsGpu final
    {
        std::array<float, 4> minimum{}, maximum{};
    };

    struct ClusterGridGpu final
    {
        std::array<std::uint32_t, 4> cells{};  // XYZ dimensions, followed by EClusterDepth.
        std::array<std::uint32_t, 4> limits{}; // Cell count, index capacity, and two reserved values.
        std::array<float, 4> position_near{}, direction_far{};
        std::array<float, 16> world_to_clip{};
    };

    struct ClusterStatusGpu final
    {
        std::uint32_t required_indices = 0; // Saturates at UINT32_MAX.
        std::uint32_t flags = 0;            // 1: capacity, 2: invalid projection, 4: saturated sum.
        std::uint32_t reserved[2]{};
    };

    static_assert(sizeof(ClusterBoundsGpu) == 32 && sizeof(ClusterGridGpu) == 128);
    static_assert(sizeof(ClusterStatusGpu) == 16);
}
