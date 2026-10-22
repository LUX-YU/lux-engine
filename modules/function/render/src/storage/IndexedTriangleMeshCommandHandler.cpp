#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshCommandHandler.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshStorage.hpp>

namespace lux::render::detail
{
    template <class Response> static exec::task<Response> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<RenderResult<MeshId>> IndexedTriangleMeshCommandHandler::uploadMesh(CreateMesh::Request&& input) noexcept
    {
        auto storage = resources_.borrow<IndexedTriangleMeshStorage>(input.storage);
        if (!storage)
        {
            return rejected<RenderResult<MeshId>>(storage.error());
        }
        return render::upload(**storage, std::move(input));
    }

    void IndexedTriangleMeshCommandHandler::releaseMesh(
        const ReleaseMesh::Request& input,
        ReleaseMesh::Response& response
    ) noexcept
    {
        auto storage = resources_.borrow<IndexedTriangleMeshStorage>(input.storage);
        if (!storage)
        {
            response.error = storage.error();
            return;
        }
        const auto released = (*storage)->release(input);
        response.error = released ? RenderError{} : released.error();
    }

} // namespace lux::render::detail
