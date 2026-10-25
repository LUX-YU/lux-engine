#pragma once

#include <lux/engine/render/extensions/storage/ResourceRegistry.hpp>

#include <lux/engine/render/extensions/CommandMethod.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>

#include <lux/engine/render/extensions/storage/IndexedTriangleMesh.hpp>
#include <lux/engine/render/extensions/storage/StorageFactory.hpp>

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
        // Factory registration does not construct an object. Activation borrows the one resource directory.
        static IndexedTriangleMeshCommandHandler* create(ResourceRegistry& resources) noexcept
        {
            return new IndexedTriangleMeshCommandHandler(resources);
        }

        using CreateMeshSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<MeshId>>>(), create_mesh_response_));

        static constexpr std::string_view name = "lux.render.storage.IndexedTriangleMeshCommandHandler";

        explicit IndexedTriangleMeshCommandHandler(ResourceRegistry& resources) noexcept : resources_(resources) {}

        CreateMeshSender createMesh(CreateMesh::Request&& input) noexcept
        {
            return uploadMesh(std::move(input)) | stdexec::then(create_mesh_response_);
        }

        void releaseMesh(const ReleaseMesh::Request&, ReleaseMesh::Response&) noexcept;
        using Bindings = std::tuple<
            CommandMethod<CreateMesh, &IndexedTriangleMeshCommandHandler::createMesh>,
            CommandMethod<ReleaseMesh, &IndexedTriangleMeshCommandHandler::releaseMesh>>;

    private:
        exec::task<RenderResult<MeshId>> uploadMesh(CreateMesh::Request&&) noexcept;

        ResourceRegistry& resources_;
    };

} // namespace lux::render::detail
