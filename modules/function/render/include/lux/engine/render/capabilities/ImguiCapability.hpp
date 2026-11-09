#pragma once

#include <lux/engine/render/commands/ImageCommands.hpp>
#include <lux/engine/render/capabilities/CapabilityDeclaration.hpp>

namespace lux::render
{
    struct ImguiFrameData;

    // Immediate-mode UI output. Fonts, sampled-image storage and descriptors are implementation details.
    struct ImguiCapability final
    {
        using PrerequisiteCapabilities = std::tuple<>;
        using Commands = std::tuple<
            ImageCommands::Upload,
            ImageCommands::CloneRegion,
            ImageCommands::Release
        >;
        using PerFrameData = std::tuple<ImguiFrameData>;
    };
} // namespace lux::render
