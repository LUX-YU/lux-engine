#include <lux/engine/systems/render/texture/TextureUploadDefinition.hpp>

namespace lux::engine::render
{
    TextureUploadDefinition::UploadCommand::Request TextureUploadDefinition::uploadRequest(
        lux::render::CapabilityImplementationId implementation,
        const Input& data
    ) noexcept
    {
        return {implementation, {data.info(), {data.pixels().data(), data.pixels().size()}}};
    }

    TextureUploadDefinition::Output TextureUploadDefinition::result(const UploadCommand::Response& response) noexcept
    {
        return response.texture;
    }

    TextureUploadDefinition::ReleaseCommand::Request TextureUploadDefinition::releaseRequest(Output resource) noexcept
    {
        return {resource};
    }

    template class RenderUpload<TextureUploadDefinition>;
} // namespace lux::engine::render
