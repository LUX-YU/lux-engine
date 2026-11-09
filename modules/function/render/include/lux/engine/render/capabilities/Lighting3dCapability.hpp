#pragma once

#include <lux/engine/render/commands/Lighting3dCommands.hpp>
#include <lux/engine/render/capabilities/Mesh3dCapability.hpp>
#include <lux/engine/render/inputs/DirectLightInputs.hpp>

namespace lux::render
{
    // Lighting for a selected Mesh3D contribution. Prerequisites describe capability dependencies, not Pass ordering.
    struct Lighting3dCapability final
    {
        using PrerequisiteCapabilities = std::tuple<Mesh3dCapability>;
        using Commands = std::tuple<
            Lighting3dCommands::CreateLights,
            Lighting3dCommands::ReleaseLight
        >;
        using PerFrameData = std::tuple<LightUpdates, CameraInput>;
    };
} // namespace lux::render
