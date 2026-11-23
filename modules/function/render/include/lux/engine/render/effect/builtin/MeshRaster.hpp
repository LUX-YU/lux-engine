#pragma once

#include <lux/engine/render/effect/EffectNodeDefinition.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    // Camera rasterization with cooked forward or depth-only material programs.
    namespace MeshRaster
    {
        inline constexpr std::string_view capability = "lux.render.mesh_raster";
        inline constexpr std::string_view forward = "lux.render.mesh_raster.forward";
        inline constexpr std::string_view forward_clockwise = "lux.render.mesh_raster.forward.clockwise";
        inline constexpr std::string_view depth_only = "lux.render.mesh_raster.depth";
        inline constexpr std::string_view depth_only_clockwise = "lux.render.mesh_raster.depth.clockwise";
        inline constexpr std::uint32_t version = 1;
        inline constexpr auto color = effectPortSemantic("color");
        inline constexpr auto depth = effectPortSemantic("depth");

        [[nodiscard]] LUX_RENDER_PUBLIC EffectNodeDefinition definition() noexcept;
    }
}
