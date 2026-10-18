#pragma once

#include <lux/engine/render/protocol/CommandMethod.hpp>

#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/Instance.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class InstanceStorage;

    class InstanceCommandHandler final
    {
    public:
        static constexpr std::string_view name = "lux.render.vulkan.InstanceCommandHandler";

        explicit InstanceCommandHandler(DeviceSession& value) noexcept : device_(value) {}

        void createInstance(const CreateInstance::Request&, CreateInstance::Response&) noexcept;
        void updateTransforms(const UpdateTransforms::Request&, UpdateTransforms::Response&) noexcept;
        void queryInstance(const QueryInstance::Request&, QueryInstance::Response&) noexcept;
        void releaseInstance(const ReleaseInstance::Request&, ReleaseInstance::Response&) noexcept;
        using Bindings = std::tuple<
            CommandMethod<CreateInstance, &InstanceCommandHandler::createInstance>,
            CommandMethod<UpdateTransforms, &InstanceCommandHandler::updateTransforms>,
            CommandMethod<QueryInstance, &InstanceCommandHandler::queryInstance>,
            CommandMethod<ReleaseInstance, &InstanceCommandHandler::releaseInstance>>;

    private:
        DeviceSession& device_;
    };

} // namespace lux::render::vulkan

namespace lux::render
{
    template <class Declaration> struct StorageProvider;

    template <> struct StorageProvider<render::InstanceStorage> final
    {
        using Storage = vulkan::InstanceStorage;
        using Handler = vulkan::InstanceCommandHandler;

        [[nodiscard]] static std::unique_ptr<Handler> createHandler(vulkan::DeviceSession& device) noexcept
        {
            return std::make_unique<Handler>(device);
        }

        [[nodiscard]] static StorageFactory factory() noexcept;
        [[nodiscard]] static CppType implementationType() noexcept;
    };
} // namespace lux::render
