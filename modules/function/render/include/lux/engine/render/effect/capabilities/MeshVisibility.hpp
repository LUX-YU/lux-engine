#pragma once

#include <lux/engine/render/effect/EffectCapability.hpp>
#include <lux/engine/render/effect/resources/VisibleDraws.hpp>
#include <lux/engine/render/resource/instance/Requirement.hpp>
#include <lux/engine/render/resource/transform/Binding.hpp>
#include <lux/engine/render/resource/view/Parameters.hpp>
#include <array>

namespace lux::render
{
    namespace MeshVisibilityPorts
    {
        inline constexpr auto draws = outputPort("draws", {VisibleDraws::type, 1});
    }

    namespace capabilities
    {
        inline constexpr std::array kMeshVisibilityPorts{
            MeshVisibilityPorts::draws
        };

        inline constexpr std::array kMeshVisibilityResources{kSceneMeshInstances, kSceneTransforms, kViewParameters};

        inline constexpr EffectCapability MeshVisibility{
            .id = graph::nodeTypeId("lux.render.mesh_visibility"),
            .canonical_name = "lux.render.mesh_visibility",
            .display_name = "Mesh Visibility",
            .ports = kMeshVisibilityPorts,
            .resources = kMeshVisibilityResources
        };
    }
}
