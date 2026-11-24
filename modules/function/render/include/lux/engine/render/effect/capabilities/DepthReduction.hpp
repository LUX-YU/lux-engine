#pragma once

#include <lux/engine/render/effect/EffectCapability.hpp>
#include <lux/engine/render/effect/resources/DepthPyramid.hpp>
#include <array>

namespace lux::render
{
    namespace DepthReductionPorts
    {
        inline constexpr auto depth = inputPort("depth", {DepthPyramid::depth_type, 1}, true);
        inline constexpr auto current = outputPort("current", {DepthPyramid::pyramid_type, 1});
        inline constexpr auto previous = outputPort("previous", {DepthPyramid::pyramid_type, 1});
    }

    namespace capabilities
    {
        inline constexpr std::array kDepthReductionPorts{
            DepthReductionPorts::depth, DepthReductionPorts::current, DepthReductionPorts::previous
        };

        inline constexpr EffectCapability DepthReduction{
            .id = graph::nodeTypeId("lux.render.depth_pyramid"),
            .canonical_name = "lux.render.depth_pyramid",
            .display_name = "Depth Pyramid",
            .ports = kDepthReductionPorts
        };
    }
}
