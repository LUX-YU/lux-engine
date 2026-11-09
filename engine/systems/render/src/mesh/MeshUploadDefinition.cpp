#include <lux/engine/systems/render/mesh/MeshUploadDefinition.hpp>

namespace lux::engine::render
{
    MeshUploadDefinition::UploadCommand::Request MeshUploadDefinition::uploadRequest(
        lux::render::CapabilityImplementationId implementation,
        const Input& data
    ) noexcept
    {
        return {implementation, &data};
    }

    MeshUploadDefinition::Output MeshUploadDefinition::result(const UploadCommand::Response& response) noexcept
    {
        return response.mesh;
    }

    MeshUploadDefinition::ReleaseCommand::Request MeshUploadDefinition::releaseRequest(Output resource) noexcept
    {
        return {resource};
    }

    template class RenderUpload<MeshUploadDefinition>;
} // namespace lux::engine::render
