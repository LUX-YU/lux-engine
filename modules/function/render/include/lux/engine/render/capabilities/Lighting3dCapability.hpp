#pragma once

#include <lux/engine/render/resources/lighting/Resource.hpp>

#include <lux/engine/render/capabilities/Mesh3dCapability.hpp>

namespace lux::render
{
    // Lighting for a selected Mesh3D contribution. Prerequisites describe capability dependencies, not Pass ordering.
    struct Lighting3dCapability final
    {
        using PrerequisiteCapabilities = std::tuple<Mesh3dCapability>;
        using Commands = std::tuple<>;
        using Resources = std::tuple<LightResource>;
    };
} // namespace lux::render
