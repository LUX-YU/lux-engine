#pragma once

#include <lux/engine/render/effect/CapabilityDefinition.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    // Camera rasterization with cooked forward or depth-only material programs.
    struct MeshRaster final
    {
        static constexpr std::string_view capability = "lux.render.mesh_raster";
        static constexpr std::string_view forward = "lux.render.mesh_raster.forward";
        static constexpr std::string_view forward_clockwise = "lux.render.mesh_raster.forward.clockwise";
        static constexpr std::string_view depth_only = "lux.render.mesh_raster.depth";
        static constexpr std::string_view depth_only_clockwise = "lux.render.mesh_raster.depth.clockwise";
        static constexpr std::uint32_t version = 1;
        static constexpr auto color = effectPortSemantic("color");
        static constexpr auto depth = effectPortSemantic("depth");

        [[nodiscard]] LUX_RENDER_PUBLIC static CapabilityDefinition definition() noexcept;
    };
}
