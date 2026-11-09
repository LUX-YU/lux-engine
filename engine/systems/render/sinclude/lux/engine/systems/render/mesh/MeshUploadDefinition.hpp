#pragma once

#include <lux/engine/systems/render/visibility.h>

#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/render/commands/Mesh3dCommands.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/systems/render/RenderUpload.hpp>
#include <lux/engine/systems/render/visibility.h>

namespace lux::engine::render
{
    // Built-in indexed-triangle mesh mapping; no runtime metadata or owned state.
    struct LUX_ENGINE_SYSTEM_RENDER_PUBLIC MeshUploadDefinition final
    {
        using Asset = asset::MeshAsset;
        using Input = rdesc::Mesh;
        using Output = lux::render::MeshId;
        using UploadCommand = lux::render::Mesh3dCommands::UploadMesh;
        using ReleaseCommand = lux::render::Mesh3dCommands::ReleaseMesh;

        [[nodiscard]] static UploadCommand::Request
        uploadRequest(lux::render::CapabilityImplementationId implementation, const Input& data) noexcept;

        [[nodiscard]] static Output result(const UploadCommand::Response& response) noexcept;

        [[nodiscard]] static ReleaseCommand::Request releaseRequest(Output resource) noexcept;
    };

    using MeshUpload = RenderUpload<MeshUploadDefinition>;
} // namespace lux::engine::render
