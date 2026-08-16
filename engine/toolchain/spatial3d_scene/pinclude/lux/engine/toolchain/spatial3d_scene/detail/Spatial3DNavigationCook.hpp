#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/navigation/detour3d/NavigationDetour3D.hpp>
#include <lux/engine/toolchain/spatial3d_scene/Spatial3DAuthoringSource.hpp>

#include <string>

namespace lux::toolchain::detail
{
    /// Builds the engine-owned navigation description directly from authored
    /// Terrain. No legacy World payload or third-party binary image crosses
    /// this private Toolchain seam.
    [[nodiscard]] lux::cxx::expected<
        lux::navigation::detour3d::NavigationRegion3DDescription,
        std::string>
    cookSpatial3DNavigationRegion(
        const Spatial3DTerrainPageSource& terrain,
        const Spatial3DNavigationAgentSource& agent,
        lux::navigation::NavigationRegionId region,
        double cell_edge) noexcept;
} // namespace lux::toolchain::detail
