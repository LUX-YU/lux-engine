#include <lux/engine/render/storage/IndexedTriangleMeshCommandHandler.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshStorage.hpp>

namespace lux::render::detail
{

    void IndexedTriangleMeshCommandHandler::releaseMesh(
        IndexedTriangleMeshStorage& storage,
        const Mesh3dCommands::ReleaseMesh::Request& input,
        Mesh3dCommands::ReleaseMesh::Response& response
    ) noexcept
    {
        const auto released = storage.release(input.data);
        response.error = released ? RenderError{} : released.error();
    }
} // namespace lux::render::detail
