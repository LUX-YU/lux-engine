#pragma once

#include <lux/engine/render/protocol/CommandHandler.hpp>

#include <lux/engine/render/resource/Mesh.hpp>
#include <lux/engine/render/resource/Factory.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class MeshStorage;

    class MeshCommands final
    {
    public:
        static constexpr std::string_view name = "lux.render.vulkan.MeshCommands";
        explicit MeshCommands(DeviceSession& device) noexcept : device_(device) {}
        RenderResult<void> set(const SetMesh::Request&, Reply<SetMesh>&) noexcept;
        RenderResult<void> query(const QueryMesh::Request&, Reply<QueryMesh>&) noexcept;
        RenderResult<void> release(const ReleaseMesh::Request&, Reply<ReleaseMesh>&) noexcept;
        using Bindings = std::tuple<CommandMethod<SetMesh, &MeshCommands::set>,
            CommandMethod<QueryMesh, &MeshCommands::query>, CommandMethod<ReleaseMesh, &MeshCommands::release>>;

    private:
        DeviceSession& device_;
    };

}

namespace lux::render
{
    template <class Declaration> struct StorageProvider;

    template <> struct StorageProvider<render::MeshStorage> final
    {
        using Storage = vulkan::MeshStorage;
        using Handler = vulkan::MeshCommands;
        [[nodiscard]] static StorageFactory factory() noexcept;
        [[nodiscard]] static StorageCppType implementationType() noexcept;
    };
}
