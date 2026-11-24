#pragma once

#include <lux/engine/render/effect/EffectCapability.hpp>
#include <lux/engine/render/effect/resources/ShadowMap.hpp>
#include <lux/engine/render/effect/resources/VisibleDraws.hpp>
#include <lux/engine/render/resource/instance/Requirement.hpp>
#include <lux/engine/render/resource/transform/Binding.hpp>
#include <lux/engine/render/resource/view/Parameters.hpp>
#include <lux/engine/render/resource/lighting/Binding.hpp>
#include <array>

namespace lux::render
{
    namespace ShadowRenderingPorts
    {
        inline constexpr auto shadows = outputPort("shadows", {ShadowBundle::type, ShadowBundle::version}, UINT8_MAX);
        inline constexpr auto draws = outputPort("draws", {VisibleDraws::type, 1}, UINT8_MAX);
    }

    namespace capabilities
    {
        inline constexpr std::array kShadowRenderingPorts{
            ShadowRenderingPorts::shadows, ShadowRenderingPorts::draws
        };

        inline constexpr std::array kShadowRenderingResources{kSceneMeshInstances, kSceneTransforms, kViewParameters, kSceneLights};

        inline constexpr EffectCapability ShadowRendering{
            .id = graph::nodeTypeId("lux.render.shadow"),
            .canonical_name = "lux.render.shadow",
            .display_name = "Shadow",
            .ports = kShadowRenderingPorts,
            .resources = kShadowRenderingResources
        };
    }
}
