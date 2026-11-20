#pragma once

#include <lux/engine/description/GBufferLayout.hpp>
#include <lux/engine/render/effect/CapabilityDefinition.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    struct GBuffer final
    {
        static constexpr std::string_view capability = "lux.render.gbuffer";
        static constexpr std::string_view indexed = "lux.render.gbuffer.indexed";
        static constexpr std::string_view indexed_clockwise = "lux.render.gbuffer.indexed.clockwise";
        static constexpr std::uint32_t version = 1;
        static constexpr auto surface = effectPortSemantic("surface");
        static constexpr auto draws = effectPortSemantic("draws");
        static constexpr std::string_view geometry_binding = "mesh.geometry";
        static constexpr std::string_view materials_binding = "mesh.materials";
        static constexpr std::string_view textures_binding = "mesh.textures";

        [[nodiscard]] LUX_RENDER_PUBLIC static CapabilityDefinition definition() noexcept;
    };
}
