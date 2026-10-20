#pragma once

#include <lux/engine/render/commands/CommandMethod.hpp>
#include <lux/engine/render/upload/UploadContext.hpp>

#include <lux/engine/render/resource/StorageFactory.hpp>
#include <lux/engine/render/storage/IndexedTriangleMesh.hpp>

namespace lux::render::detail
{
    class DeviceSession;
    class IndexedTriangleMeshStorage;

    class IndexedTriangleMeshCommandHandler final
    {
    private:
        static constexpr auto create_mesh_response_ = [](RenderResult<MeshId> result) noexcept -> CreateMesh::Response
        {
            if (!result)
            {
                return {result.error()};
            }
            return {{}, *result};
        };

    public:
        using CreateMeshSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<MeshId>>>(), create_mesh_response_));

        static constexpr std::string_view name = "lux.render.storage.IndexedTriangleMeshCommandHandler";

        explicit IndexedTriangleMeshCommandHandler(DeviceSession& device) noexcept : device_(device) {}

        CreateMeshSender createMesh(CreateMesh::Request&& input) noexcept
        {
            return createMeshSource(std::move(input)) | stdexec::then(create_mesh_response_);
        }

        void releaseMesh(const ReleaseMesh::Request&, ReleaseMesh::Response&) noexcept;
        using Bindings = std::tuple<
            CommandMethod<CreateMesh, &IndexedTriangleMeshCommandHandler::createMesh>,
            CommandMethod<ReleaseMesh, &IndexedTriangleMeshCommandHandler::releaseMesh>>;

    private:
        exec::task<RenderResult<MeshId>> createMeshSource(CreateMesh::Request&&) noexcept;

        DeviceSession& device_;
    };

} // namespace lux::render::detail

// Registered by this implementation TU; no concrete Handler is exposed by the SDK.
#include <lux/engine/render/runtime/RenderServer.hpp>

namespace lux::render
{
    template <>
    RenderResult<void> RenderServer::prepareStorage<IndexedTriangleMeshStorageDescriptor>(
        RegistrationBatch&,
        ConfigValue,
        bool,
        std::string_view
    ) noexcept;
}
