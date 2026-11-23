#pragma once

#include <lux/engine/render/effect/EffectNodeDefinition.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    // Camera visibility for Storage-owned triangle meshes. GPU outputs belong to the current frame.
    namespace MeshVisibility
    {
        inline constexpr std::string_view capability = "lux.render.mesh_visibility";
        inline constexpr std::string_view camera = "lux.render.mesh_visibility.camera";
        inline constexpr std::uint32_t version = 1;
        inline constexpr auto draws = effectPortSemantic("draws");
        inline constexpr auto groups = effectPortSemantic("groups");
        inline constexpr auto visible = effectPortSemantic("visible");
        inline constexpr auto commands = effectPortSemantic("commands");
        inline constexpr std::string_view draws_type = "lux.mesh.indexed-visibility";
        inline constexpr std::string_view instances_binding = "mesh.instances";
        inline constexpr std::string_view properties_binding = "mesh.properties";
        inline constexpr std::string_view alive_binding = "mesh.alive";
        inline constexpr std::string_view transforms_binding = "mesh.transforms";

        [[nodiscard]] LUX_RENDER_PUBLIC EffectNodeDefinition definition() noexcept;
    }
} // namespace lux::render
