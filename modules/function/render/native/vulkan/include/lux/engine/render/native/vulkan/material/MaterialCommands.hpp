#pragma once

#include <lux/engine/render/resource/Material.hpp>
#include <lux/engine/render/resource/Factory.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class MaterialStorage;
    template <class Declaration> struct StorageProvider;

    class MaterialCommands final
    {
    public:
        static constexpr std::string_view name = "lux.render.vulkan.MaterialCommands";
        explicit MaterialCommands(DeviceSession& device) noexcept : device_(device) {}
        RenderResult<void> set(const SetMaterial::Request&, Reply<SetMaterial>&) noexcept;
        RenderResult<void> query(const QueryMaterial::Request&, Reply<QueryMaterial>&) noexcept;
        RenderResult<void> release(const ReleaseMaterial::Request&, Reply<ReleaseMaterial>&) noexcept;
        using Bindings = std::tuple<CommandMethod<SetMaterial, &MaterialCommands::set>,
            CommandMethod<QueryMaterial, &MaterialCommands::query>, CommandMethod<ReleaseMaterial, &MaterialCommands::release>>;

    private:
        DeviceSession& device_;
    };

    template <> struct StorageProvider<render::MaterialStorage> final
    {
        using Storage = MaterialStorage;
        using Handler = MaterialCommands;
        [[nodiscard]] static StorageFactory factory() noexcept;
        [[nodiscard]] static StorageCppType implementationType() noexcept;
    };
}
