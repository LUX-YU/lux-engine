#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>

namespace lux::render::vulkan
{
    static exec::task<SetMesh::Response> rejectedMesh(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<SetMesh::Response> MeshCommandHandler::setMesh(SetMesh::Request&& input) noexcept
    {
        auto storage = device_.resources().borrow<MeshStorage>(input.mesh.storage);
        if (!storage)
        {
            return rejectedMesh(storage.error());
        }
        return render::upload(**storage, std::move(input));
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

    CppType StorageProvider<render::MeshStorage>::implementationType() noexcept
    {
        return cppType<Storage>();
    }
} // namespace lux::render
