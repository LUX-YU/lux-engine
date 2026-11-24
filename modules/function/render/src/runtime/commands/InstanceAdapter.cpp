#include <lux/engine/render/assembly/BuiltinEffects.hpp>
#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <lux/engine/render/resource/instance/MeshInstanceStorage.hpp>
#include <lux/engine/render/runtime/commands/InstanceCommands.hpp>

namespace lux::render::detail
{
    namespace
    {
        void createInstance(
            MeshInstanceStorage& owner, const InstanceCommands::CreateInstance::Request& input,
            InstanceCommands::CreateInstance::Response& reply
        ) noexcept
        {
            auto result = owner.create({&input.data, 1}, {&reply.instance, 1});
            if (!result) reply = {result.error()};
            else reply.error = {};
        }

        void createInstances(
            MeshInstanceStorage& owner, const InstanceCommands::CreateInstances::Request& input,
            InstanceCommands::CreateInstances::Response& reply
        ) noexcept
        {
            if (input.data.size() > UINT32_MAX)
            {
                reply.error = {kStorageCapacity};
                return;
            }
            reply.instances.resize(input.data.size());
            auto result = owner.create(input.data, reply.instances);
            if (!result) reply = {result.error()};
            else reply.error = {};
        }

        void releaseInstance(
            MeshInstanceStorage& owner, const InstanceCommands::ReleaseInstance::Request& input, AckResponse& reply
        ) noexcept
        {
            auto result = owner.release(input.data);
            reply.error = result ? RenderError{} : result.error();
        }

        void updateInstances(
            MeshInstanceStorage& resource,
            const InstanceCommands::Update::Request& input,
            AckResponse& response
        ) noexcept
        {
            const auto result = resource.update(input.instances);
            response.error = result ? RenderError{} : result.error();
        }
    }

    std::span<const CommandBinding> meshInstanceCommands() noexcept
    {
        static const std::array commands{
            CommandBinding::declare<InstanceCommands::CreateInstance, &createInstance>(),
            CommandBinding::declare<InstanceCommands::CreateInstances, &createInstances>(),
            CommandBinding::declare<InstanceCommands::ReleaseInstance, &releaseInstance>(),
            CommandBinding::declare<InstanceCommands::Update, &updateInstances>()
        };
        return commands;
    }
}
