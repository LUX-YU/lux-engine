#pragma once

#include <lux/engine/render/runtime/commands/MaterialCommands.hpp>
#include <lux/engine/systems/render/visibility.h>

#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/systems/render/RenderUpload.hpp>
#include <lux/engine/render/commands/MaterialCommands.hpp>
#include <lux/engine/systems/render/texture/TextureUploadDefinition.hpp>

namespace lux::engine::render
{
    struct LUX_ENGINE_SYSTEM_RENDER_PUBLIC MaterialUploadDefinition final
    {
        using Asset = asset::MaterialAsset;
        using Input = lux::render::MaterialData;
        using Output = lux::render::MaterialId;
        using UploadCommand = lux::render::MaterialCommands::Create;
        using ReleaseCommand = lux::render::MaterialCommands::Release;

        [[nodiscard]] static UploadCommand::Request
        uploadRequest(lux::render::CapabilityImplementationId implementation, const Input& data) noexcept;
        [[nodiscard]] static Output result(const UploadCommand::Response& response) noexcept;
        [[nodiscard]] static ReleaseCommand::Request releaseRequest(Output resource) noexcept;

        // Asset composition belongs here; the upload template only transports already prepared input.
        [[nodiscard]] static exec::task<engine::assets::AssetResult<Output>> prepare(
            engine::assets::AssetRepository& assets,
            const Asset::data_type& data,
            RenderUpload<MaterialUploadDefinition>& upload,
            TextureUpload& textures,
            std::vector<engine::assets::AssetDataRef<void>>& dependencies
        ) noexcept;
    };

    using MaterialUpload = RenderUpload<MaterialUploadDefinition>;
}
