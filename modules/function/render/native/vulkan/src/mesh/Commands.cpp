#include <lux/engine/render/native/vulkan/mesh/MeshCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>

namespace lux::render::vulkan
{
    GpuWorkSender<SetMesh::Response> MeshCommandHandler::setMesh(SetMesh::Request&& input) noexcept
    {
        auto storage = device_.resources().borrow<MeshStorage>(input.mesh.storage);
        if (!storage)
        {
            return GpuWorkSender<SetMesh::Response>{storage.error()};
        }
        auto candidate = render::upload(**storage, input);
        if (!candidate)
        {
            return GpuWorkSender<SetMesh::Response>{candidate.error()};
        }
        return {std::move(candidate->first), std::move(candidate->second), std::move(input.source)};
    }

    void MeshCommandHandler::queryMesh(const QueryMesh::Request& input, QueryMesh::Response& response) noexcept
    {
        auto storage = device_.resources().tryGet<render::MeshStorage>(input.storage);
        if (!storage)
        {
            response = cxx::unexpected(storage.error());
            return;
        }
        auto result = storage->query(input);
        if (!result)
        {
            response = cxx::unexpected(result.error());
            return;
        }
        response = std::move(*result);
    }

    void MeshCommandHandler::releaseMesh(const ReleaseMesh::Request& input, ReleaseMesh::Response& response) noexcept
    {
        auto storage = device_.resources().borrow<MeshStorage>(input.mesh.storage);
        if (!storage)
        {
            response = cxx::unexpected(storage.error());
            return;
        }
        if (auto result = (*storage)->release(input.mesh, input.revision); !result)
        {
            response = cxx::unexpected(result.error());
            return;
        }
        response = Ack{};
    }

} // namespace lux::render::vulkan

namespace lux::render
{
    StorageFactory StorageProvider<render::MeshStorage>::factory() noexcept
    {
        return vulkan::meshStorageFactory();
    }

    StorageCppType StorageProvider<render::MeshStorage>::implementationType() noexcept
    {
        return storageCppType<Storage>();
    }
} // namespace lux::render
