#pragma once

#include <lux/engine/render/resources/texture/Resource.hpp>

#include <lux/engine/render/capabilities/CapabilityDeclaration.hpp>

namespace lux::render
{
    struct ImguiFrameData;

    // Immediate-mode UI output. Fonts, sampled-image storage and descriptors are implementation details.
    struct ImguiCapability final
    {
        using PrerequisiteCapabilities = std::tuple<>;
        using Commands = std::tuple<>;
        using Resources = std::tuple<TextureResource>;
    };
} // namespace lux::render
