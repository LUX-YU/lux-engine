#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshCommandHandler.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshStorage.hpp>

namespace lux::render::detail
{
    template <class Response> static exec::task<Response> rejected(RenderError error) noexcept
    {
        co_return cxx::unexpected(error);
    }

    exec::task<RenderResult<MeshId>> IndexedTriangleMeshCommandHandler::createMeshSource(CreateMesh::Request&& input
    ) noexcept
    {
        auto storage = device_.resources().borrow<IndexedTriangleMeshStorage>(input.storage);
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
        auto storage = device_.resources().borrow<IndexedTriangleMeshStorage>(input.storage);
        if (!storage)
        {
            response.error = storage.error();
            return;
        }
        const auto released = (*storage)->release(input);
        response.error = released ? RenderError{} : released.error();
    }

} // namespace lux::render::detail

#include <lux/engine/render/runtime/Registration.hpp>

namespace lux::render
{
    template <>
    RenderResult<void> RenderServer::prepareStorage<IndexedTriangleMeshStorageDescriptor>(
        RegistrationBatch& batch,
        ConfigValue configuration,
        bool use_existing,
        std::string_view handler_key
    ) noexcept
    {
        return prepareStorageDefinition<
            IndexedTriangleMeshStorageDescriptor,
            detail::IndexedTriangleMeshStorage,
            detail::IndexedTriangleMeshCommandHandler>(
            batch,
            detail::meshStorageFactory(),
            std::move(configuration),
            use_existing,
            handler_key
        );
    }

    template LUX_RENDER_PUBLIC RenderResult<StorageTypeId> RenderServer::registerStorage<
        IndexedTriangleMeshStorageDescriptor>(std::string_view) noexcept;
} // namespace lux::render
