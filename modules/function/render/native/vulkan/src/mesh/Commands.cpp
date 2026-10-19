#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshCommandHandler.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>

namespace lux::render::vulkan
{
    template <class Response> static exec::task<Response> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<RenderResult<MeshId>> MeshCommandHandler::createMeshSource(CreateMesh::Request&& input) noexcept
    {
        auto storage = device_.resources().borrow<MeshStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<MeshId>>(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    void MeshCommandHandler::releaseMesh(const ReleaseMesh::Request& input, ReleaseMesh::Response& response) noexcept
    {
        auto storage = device_.resources().borrow<MeshStorage>(input.storage);
        if (!storage)
        {
            response.error = storage.error();
            return;
        }
        const auto released = (*storage)->release(input);
        response.error = released ? RenderError{} : released.error();
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
