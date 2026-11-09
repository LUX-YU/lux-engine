#pragma once

#include <lux/engine/systems/render/visibility.h>

#include <lux/engine/render/commands/ImageCommands.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/systems/render/RenderUpload.hpp>

namespace lux::engine::render
{
    struct LUX_ENGINE_SYSTEM_RENDER_PUBLIC TextureUploadDefinition final
    {
        using Asset = asset::TextureAsset;
        using Input = rdesc::Texture;
        using Output = lux::render::TextureId;
        using UploadCommand = lux::render::ImageCommands::Upload;
        using ReleaseCommand = lux::render::ImageCommands::Release;

        [[nodiscard]] static UploadCommand::Request
        uploadRequest(lux::render::CapabilityImplementationId implementation, const Input& data) noexcept;
        [[nodiscard]] static Output result(const UploadCommand::Response& response) noexcept;
        [[nodiscard]] static ReleaseCommand::Request releaseRequest(Output resource) noexcept;
    };

    using TextureUpload = RenderUpload<TextureUploadDefinition>;
} // namespace lux::engine::render
