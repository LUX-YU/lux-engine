#pragma once

#include <lux/engine/render/effect/EffectCapability.hpp>
#include <lux/engine/render/effect/resources/GBuffer.hpp>
#include <lux/engine/render/effect/resources/VisibleDraws.hpp>
#include <lux/engine/render/resource/instance/Requirement.hpp>
#include <lux/engine/render/resource/transform/Binding.hpp>
#include <lux/engine/render/resource/view/Parameters.hpp>
#include <array>

namespace lux::render
{
    namespace DeferredGeometryPorts
    {
        inline constexpr auto surface = outputPort("surface", {GBufferBundle::type, GBufferBundle::version}, UINT8_MAX);
        inline constexpr auto draws = outputPort("draws", {VisibleDraws::type, 1}, UINT8_MAX);
    }

    namespace capabilities
    {
        inline constexpr std::array kDeferredGeometryPorts{
            DeferredGeometryPorts::surface, DeferredGeometryPorts::draws
        };

        inline constexpr std::array kDeferredGeometryResources{kSceneMeshInstances, kSceneTransforms, kViewParameters};

        inline constexpr EffectCapability DeferredGeometry{
            .id = graph::nodeTypeId("lux.render.gbuffer"),
            .canonical_name = "lux.render.gbuffer",
            .display_name = "GBuffer",
            .ports = kDeferredGeometryPorts,
            .resources = kDeferredGeometryResources
        };
    }
}
