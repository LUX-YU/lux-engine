#pragma once

#include <lux/engine/render/commands/CommandMethod.hpp>

#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/MeshInstance.hpp>

namespace lux::render::vulkan
{
    class DeviceSession;
    class MeshInstanceStorage;

    class MeshInstanceCommandHandler final
    {
    public:
        static constexpr std::string_view name = "lux.render.vulkan.MeshInstanceCommandHandler";

        explicit MeshInstanceCommandHandler(DeviceSession& value) noexcept : device_(value) {}

        void createInstance(const CreateMeshInstance::Request&, CreateMeshInstance::Response&) noexcept;
        void updateTransforms(const UpdateTransforms::Request&, UpdateTransforms::Response&) noexcept;
        void queryInstance(const QueryMeshInstance::Request&, QueryMeshInstance::Response&) noexcept;
        void releaseInstance(const ReleaseMeshInstance::Request&, ReleaseMeshInstance::Response&) noexcept;
        using Bindings = std::tuple<
            CommandMethod<CreateMeshInstance, &MeshInstanceCommandHandler::createInstance>,
            CommandMethod<UpdateTransforms, &MeshInstanceCommandHandler::updateTransforms>,
            CommandMethod<QueryMeshInstance, &MeshInstanceCommandHandler::queryInstance>,
            CommandMethod<ReleaseMeshInstance, &MeshInstanceCommandHandler::releaseInstance>>;

    private:
        DeviceSession& device_;
    };

} // namespace lux::render::vulkan

namespace lux::render
{
    template <class Declaration> struct StorageProvider;

    template <> struct StorageProvider<render::MeshInstanceStorage> final
    {
        using Storage = vulkan::MeshInstanceStorage;
        using Handler = vulkan::MeshInstanceCommandHandler;

        [[nodiscard]] static std::unique_ptr<Handler> createHandler(vulkan::DeviceSession& device) noexcept
        {
            return std::make_unique<Handler>(device);
        }

        [[nodiscard]] static StorageFactory factory() noexcept;
        [[nodiscard]] static CppType implementationType() noexcept;
    };
} // namespace lux::render
