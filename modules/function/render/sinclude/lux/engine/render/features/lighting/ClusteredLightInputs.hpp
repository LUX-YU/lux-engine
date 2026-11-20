#pragma once

#include <lux/engine/render/capabilities/ClusterGrid.hpp>
#include <string_view>

namespace lux::render
{
    inline constexpr std::string_view kClusterGrid = "lux.light.cluster.grid.forward-z.v1";
    inline constexpr std::string_view kClusterOffsets = "lux.light.cluster.exclusive-offsets.v1";
    // Legacy packed index: low 31 bits dense index, bit 31 selects spot instead of point.
    inline constexpr std::string_view kClusterIndices = "lux.light.cluster.point-spot-indices.v1";
    inline constexpr std::string_view kClusterStatus = "lux.light.cluster.status.v1";
} // namespace lux::render
