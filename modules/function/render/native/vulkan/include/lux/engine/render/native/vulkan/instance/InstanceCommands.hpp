#pragma once

#include <lux/engine/render/protocol/CommandHandler.hpp>

#include <lux/engine/render/resource/Instance.hpp>
#include <lux/engine/render/resource/Factory.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class InstanceStorage;
    template <class Declaration> struct StorageProvider;

    class InstanceCommands final
    {
    public:
        static constexpr std::string_view name = "lux.render.vulkan.InstanceCommands";
        explicit InstanceCommands(DeviceSession& value) noexcept : device_(value) {}
        RenderResult<void> create(const CreateInstance::Request&, Reply<CreateInstance>&) noexcept;
        RenderResult<void> update(const UpdateTransforms::Request&, Reply<UpdateTransforms>&) noexcept;
        RenderResult<void> query(const QueryInstance::Request&, Reply<QueryInstance>&) noexcept;
        RenderResult<void> release(const ReleaseInstance::Request&, Reply<ReleaseInstance>&) noexcept;
        using Bindings = std::tuple<CommandMethod<CreateInstance, &InstanceCommands::create>,
            CommandMethod<UpdateTransforms, &InstanceCommands::update>, CommandMethod<QueryInstance, &InstanceCommands::query>,
            CommandMethod<ReleaseInstance, &InstanceCommands::release>>;

    private:
        DeviceSession& device_;
    };

    template <> struct StorageProvider<render::InstanceStorage> final
    {
        using Storage = InstanceStorage;
        using Handler = InstanceCommands;
        [[nodiscard]] static StorageFactory factory() noexcept;
        [[nodiscard]] static StorageCppType implementationType() noexcept;
    };
}
