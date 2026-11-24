#pragma once

#include <lux/engine/render/effect/EffectCapability.hpp>
#include <lux/engine/render/effect/resources/Lighting.hpp>
#include <lux/engine/render/effect/resources/GBuffer.hpp>
#include <lux/engine/render/effect/resources/ShadowMap.hpp>
#include <lux/engine/render/resource/view/Parameters.hpp>
#include <lux/engine/render/resource/lighting/Binding.hpp>
#include <array>

namespace lux::render
{
    namespace DeferredLightingPorts
    {
        inline constexpr auto surface = inputPort("surface", {GBufferBundle::type, GBufferBundle::version}, true);
        inline constexpr auto shadows = inputPort("shadows", {ShadowBundle::type, ShadowBundle::version});
        inline constexpr auto hdr = outputPort("hdr_color", {HdrColorBundle::type, HdrColorBundle::version}, UINT8_MAX);
        inline constexpr auto cluster_lists = outputPort("cluster_lists", {ClusterListBundle::type, ClusterListBundle::version}, UINT8_MAX);
    }

    namespace capabilities
    {
        inline constexpr std::array kDeferredLightingPorts{
            DeferredLightingPorts::surface, 
            DeferredLightingPorts::shadows, 
            DeferredLightingPorts::hdr, 
            DeferredLightingPorts::cluster_lists
        };

        inline constexpr std::array kDeferredLightingResources{kViewParameters, kSceneLights};

        inline constexpr EffectCapability DeferredLighting{
            .id             = graph::nodeTypeId("lux.render.lighting"),
            .canonical_name = "lux.render.lighting",
            .display_name   = "Lighting",
            .ports          = kDeferredLightingPorts,
            .resources      = kDeferredLightingResources
        };
    }
}
