#pragma once

#include <lux/engine/render/commands/Mesh3dCommands.hpp>
#include <lux/engine/render/commands/ImageCommands.hpp>
#include <lux/engine/render/commands/MaterialCommands.hpp>
#include <lux/engine/render/capabilities/CapabilityDeclaration.hpp>
#include <lux/engine/render/inputs/CameraInputs.hpp>
#include <lux/engine/render/inputs/RasterInstanceInputs.hpp>

namespace lux::render
{
    // Visible three-dimensional geometry. Concrete storage and shader layout belong to its implementation.
    struct Mesh3dCapability final
    {
        using PrerequisiteCapabilities = std::tuple<>;
        using Commands = std::tuple<
            Mesh3dCommands::UploadMesh,
            Mesh3dCommands::ReleaseMesh,
            ImageCommands::Upload,
            ImageCommands::CloneRegion,
            ImageCommands::Release,
            MaterialCommands::Create,
            MaterialCommands::Update,
            MaterialCommands::Release,
            Mesh3dCommands::CreateInstance,
            Mesh3dCommands::CreateInstances,
            Mesh3dCommands::ReleaseInstance
        >;
        using PerFrameData = std::tuple<TransformData, MeshInstanceUpdates, CameraInput>;
    };
} // namespace lux::render
