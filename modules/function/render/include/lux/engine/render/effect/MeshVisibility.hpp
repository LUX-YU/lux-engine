#pragma once

#include <lux/engine/render/effect/CapabilityDefinition.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    // Camera visibility for Storage-owned triangle meshes. GPU outputs belong to the current frame.
    struct MeshVisibility final
    {
        static constexpr std::string_view capability = "lux.render.mesh_visibility";
        static constexpr std::string_view camera = "lux.render.mesh_visibility.camera";
        static constexpr std::uint32_t version = 1;
        static constexpr auto draws = effectPortSemantic("draws");
        static constexpr auto groups = effectPortSemantic("groups");
        static constexpr auto visible = effectPortSemantic("visible");
        static constexpr auto commands = effectPortSemantic("commands");
        static constexpr std::string_view draws_type = "lux.mesh.indexed-visibility";
        static constexpr std::string_view instances_binding = "mesh.instances";
        static constexpr std::string_view properties_binding = "mesh.properties";
        static constexpr std::string_view alive_binding = "mesh.alive";
        static constexpr std::string_view transforms_binding = "mesh.transforms";

        [[nodiscard]] LUX_RENDER_PUBLIC static CapabilityDefinition definition() noexcept;
    };
} // namespace lux::render
