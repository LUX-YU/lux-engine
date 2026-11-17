#pragma once

#include <lux/engine/render/resources/mesh/Resource.hpp>
#include <lux/engine/render/resources/texture/Resource.hpp>
#include <lux/engine/render/resources/material/Resource.hpp>
#include <lux/engine/render/resources/instance/Resource.hpp>

#include <lux/engine/render/capabilities/CapabilityDeclaration.hpp>

namespace lux::render
{
    // Visible three-dimensional geometry. Concrete storage and shader layout belong to its implementation.
    struct Mesh3dCapability final
    {
        using PrerequisiteCapabilities = std::tuple<>;
        using Commands = std::tuple<>;
        using Resources = std::tuple<MeshResource, TextureResource, MaterialResource, InstanceResource>;
    };
} // namespace lux::render
