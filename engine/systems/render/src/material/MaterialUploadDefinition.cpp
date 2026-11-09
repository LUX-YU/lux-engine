#include <lux/engine/systems/render/material/MaterialUploadDefinition.hpp>

namespace lux::engine::render
{
    MaterialUploadDefinition::UploadCommand::Request
    MaterialUploadDefinition::uploadRequest(
        lux::render::CapabilityImplementationId implementation, const Input& data
    ) noexcept
    {
        return {implementation, data};
    }

    MaterialUploadDefinition::Output
    MaterialUploadDefinition::result(const UploadCommand::Response& response) noexcept
    {
        return response.material;
    }

    MaterialUploadDefinition::ReleaseCommand::Request
    MaterialUploadDefinition::releaseRequest(Output resource) noexcept
    {
        return {resource};
    }

    exec::task<engine::assets::AssetResult<MaterialUploadDefinition::Output>>
    MaterialUploadDefinition::prepare(
        engine::assets::AssetRepository& assets,
        const Asset::data_type& data,
        MaterialUpload& upload,
        TextureUpload& textures,
        std::vector<engine::assets::AssetDataRef<void>>& dependencies
    ) noexcept
    {
        lux::render::MaterialData material;
        material.program = data.programs;
        material.parameters = data.programs.parameter_defaults;
        material.textures.reserve(data.texture_slot_ids.size());
        dependencies.reserve(data.texture_slot_ids.size());
        for (auto id : data.texture_slot_ids)
        {
            if (id.isNull())
            {
                material.textures.emplace_back(); // Native material storage maps an absent texture to white.
                continue;
            }
            auto asset = assets.acquire<asset::TextureAsset>(id);
            if (!asset)
            {
                co_return cxx::unexpected(asset.error());
            }
            auto texture = co_await assets.use<TextureUploadDefinition>(*asset, textures);
            if (!texture)
            {
                co_return cxx::unexpected(texture.error());
            }
            material.textures.push_back(**texture);
            dependencies.emplace_back(*texture);
        }
        co_return co_await upload.upload(material);
    }

    template class RenderUpload<MaterialUploadDefinition>;
}
