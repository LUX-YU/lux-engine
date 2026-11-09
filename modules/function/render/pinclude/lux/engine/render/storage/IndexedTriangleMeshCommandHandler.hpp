#pragma once
#include <lux/engine/render/storage/IndexedTriangleMeshStorage.hpp>

#include <lux/engine/render/commands/Mesh3dCommands.hpp>

#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>

#include <lux/engine/render/extensions/storage/IndexedTriangleMesh.hpp>
#include <lux/engine/render/extensions/storage/StorageFactory.hpp>

namespace lux::render
{
    class ResourceRegistry;
}

namespace lux::render::detail
{
    class DeviceSession;
    class IndexedTriangleMeshStorage;

    class IndexedTriangleMeshCommandHandler final
    {
    private:
        static exec::task<RenderResult<MeshId>> rejectMesh(RenderError error) noexcept
        {
            co_return cxx::unexpected(error);
        }

        static constexpr auto create_mesh_response_ = [](RenderResult<MeshId> result
                                                      ) noexcept -> Mesh3dCommands::UploadMesh::Response
        {
            if (!result)
            {
                return {result.error()};
            }
            return {{}, *result};
        };

    public:
        // Stateless typed methods; the Server supplies the selected Storage at dispatch.
        static IndexedTriangleMeshCommandHandler* create(ResourceRegistry&) noexcept
        {
            return new IndexedTriangleMeshCommandHandler;
        }

        using UploadMeshSender =
            decltype(stdexec::then(std::declval<exec::task<RenderResult<MeshId>>>(), create_mesh_response_));

        static constexpr std::string_view name = "lux.render.storage.IndexedTriangleMeshCommandHandler";

        UploadMeshSender
        createMesh(IndexedTriangleMeshStorage& storage, Mesh3dCommands::UploadMesh::Request&& input) noexcept
        {
            if (!input.mesh)
            {
                return rejectMesh(RenderError{kCommandContract}) | stdexec::then(create_mesh_response_);
            }
            return render::upload(storage, *input.mesh) | stdexec::then(create_mesh_response_);
        }

        void releaseMesh(
            IndexedTriangleMeshStorage& storage,
            const Mesh3dCommands::ReleaseMesh::Request& input,
            Mesh3dCommands::ReleaseMesh::Response& response
        ) noexcept;
    };

} // namespace lux::render::detail
