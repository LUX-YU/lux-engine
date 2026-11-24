#pragma once

#include <lux/engine/render/effect/EffectCapability.hpp>
#include <lux/engine/render/effect/resources/Color.hpp>
#include <lux/engine/render/effect/resources/DepthPyramid.hpp>
#include <lux/engine/render/resource/instance/Requirement.hpp>
#include <lux/engine/render/resource/transform/Binding.hpp>
#include <lux/engine/render/resource/view/Parameters.hpp>
#include <array>

namespace lux::render
{
    namespace ForwardGeometryPorts
    {
        inline constexpr auto color = outputPort("color", {kDisplayColorType, 1}, UINT8_MAX);
        inline constexpr auto depth = outputPort("depth", {DepthPyramid::depth_type, 1}, UINT8_MAX);
    }

    namespace capabilities
    {
        inline constexpr std::array kForwardGeometryPorts{
            ForwardGeometryPorts::color, ForwardGeometryPorts::depth
        };

        inline constexpr std::array kForwardGeometryResources{kSceneMeshInstances, kSceneTransforms, kViewParameters};

        inline constexpr EffectCapability ForwardGeometry{
            .id = graph::nodeTypeId("lux.render.mesh_raster"),
            .canonical_name = "lux.render.mesh_raster",
            .display_name = "Mesh Raster",
            .ports = kForwardGeometryPorts,
            .resources = kForwardGeometryResources
        };
    }
}
