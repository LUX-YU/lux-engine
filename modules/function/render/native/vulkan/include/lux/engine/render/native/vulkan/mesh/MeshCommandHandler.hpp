#pragma once

#include <lux/engine/render/commands/CommandMethod.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>

#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/Mesh.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class MeshStorage;

    class MeshCommandHandler final
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

        static constexpr std::string_view name = "lux.render.vulkan.MeshCommandHandler";

        explicit MeshCommandHandler(DeviceSession& device) noexcept : device_(device) {}

        CreateMeshSender createMesh(CreateMesh::Request&& input) noexcept
        {
            return createMeshSource(std::move(input)) | stdexec::then(create_mesh_response_);
        }

        void releaseMesh(const ReleaseMesh::Request&, ReleaseMesh::Response&) noexcept;
        using Bindings = std::tuple<
            CommandMethod<CreateMesh, &MeshCommandHandler::createMesh>,
            CommandMethod<ReleaseMesh, &MeshCommandHandler::releaseMesh>>;

    private:
        exec::task<RenderResult<MeshId>> createMeshSource(CreateMesh::Request&&) noexcept;

        DeviceSession& device_;
    };

} // namespace lux::render::vulkan

namespace lux::render
{
    template <class Declaration> struct StorageProvider;

    template <> struct StorageProvider<render::MeshStorage> final
    {
        using Storage = vulkan::MeshStorage;
        using Handler = vulkan::MeshCommandHandler;

        [[nodiscard]] static std::unique_ptr<Handler> createHandler(vulkan::DeviceSession& device) noexcept
        {
            return std::make_unique<Handler>(device);
        }

        [[nodiscard]] static StorageFactory factory() noexcept;
        [[nodiscard]] static CppType implementationType() noexcept;
    };
} // namespace lux::render
