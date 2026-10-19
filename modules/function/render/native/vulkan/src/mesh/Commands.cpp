#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>

namespace lux::render::vulkan
{
    template <class Response> static exec::task<Response> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<CreateMesh::Response> MeshCommandHandler::createMesh(CreateMesh::Request&& input) noexcept
    {
        auto storage = device_.resources().borrow<MeshStorage>(input.storage);
        if (!storage)
        {
            return rejected<CreateMesh::Response>(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    void MeshCommandHandler::releaseMesh(const ReleaseMesh::Request& input, ReleaseMesh::Response& response) noexcept
    {
        auto storage = device_.resources().borrow<MeshStorage>(input.storage);
        if (!storage)
        {
            response = cxx::unexpected(storage.error());
            return;
        }
        const auto released = (*storage)->release(input);
        response = released ? ReleaseMesh::Response(Ack{}) : cxx::unexpected(released.error());
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
