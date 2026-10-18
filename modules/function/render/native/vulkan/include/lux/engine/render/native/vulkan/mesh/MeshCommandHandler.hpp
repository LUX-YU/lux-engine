#pragma once

#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>
#include <lux/engine/render/protocol/CommandMethod.hpp>

#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/Mesh.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class MeshStorage;

    class MeshCommandHandler final
    {
    public:
        static constexpr std::string_view name = "lux.render.vulkan.MeshCommandHandler";

        explicit MeshCommandHandler(DeviceSession& device) noexcept : device_(device) {}

        GpuWorkSender<SetMesh::Response> setMesh(SetMesh::Request&&) noexcept;
        void queryMesh(const QueryMesh::Request&, QueryMesh::Response&) noexcept;
        void releaseMesh(const ReleaseMesh::Request&, ReleaseMesh::Response&) noexcept;
        using Bindings = std::tuple<
            CommandMethod<SetMesh, &MeshCommandHandler::setMesh>,
            CommandMethod<QueryMesh, &MeshCommandHandler::queryMesh>,
            CommandMethod<ReleaseMesh, &MeshCommandHandler::releaseMesh>>;

    private:
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
        [[nodiscard]] static StorageFactory factory() noexcept;
        [[nodiscard]] static CppType implementationType() noexcept;
    };
} // namespace lux::render
