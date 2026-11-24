#include <lux/engine/render/assembly/BuiltinEffects.hpp>
#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <lux/engine/render/resource/material/MaterialStorage.hpp>
#include <lux/engine/render/runtime/commands/MaterialCommands.hpp>

namespace lux::render::detail
{
    namespace
    {
        void updateMaterial(
            MaterialStorage& owner, const MaterialCommands::Update::Request& input, AckResponse& reply
        ) noexcept
        {
            auto result = owner.update(input.data);
            reply.error = result ? RenderError{} : result.error();
        }

        void updateMaterialColors(
            MaterialStorage& storage, const MaterialCommands::UpdateColor::Request& input, AckResponse& reply
        ) noexcept
        {
            const auto updated = storage.updateColors(input.updates);
            reply.error = updated ? RenderError{} : updated.error();
        }

        exec::task<MaterialCommands::RegisterProgram::Response>
        handleRegisterProgram(MaterialStorage& resource, MaterialCommands::RegisterProgram::Request&& input) noexcept
        {
            auto registered = resource.registerProgram(std::move(input.program));
            if (!registered) co_return MaterialCommands::RegisterProgram::Response{registered.error()};
            co_return MaterialCommands::RegisterProgram::Response{{}, registered->program, registered->indexed_mesh_shader};
        }

        exec::task<AckResponse>
        prepareMeshPipelines(MaterialStorage& resource, MaterialCommands::PrepareMeshPipelines::Request&& input) noexcept
        {
            auto result = co_await resource.prepareMeshPipelines(std::move(input));
            co_return AckResponse{result ? RenderError{} : result.error()};
        }

        void handleCreateInstance(
            MaterialStorage& owner, const MaterialCommands::CreateInstance::Request& input,
            MaterialCommands::CreateInstance::Response& reply
        ) noexcept
        {
            auto result = owner.createInstance(input);
            reply = result ? MaterialCommands::CreateInstance::Response{{}, *result}
                           : MaterialCommands::CreateInstance::Response{result.error()};
        }

        void releaseInstance(
            MaterialStorage& resource, const MaterialCommands::ReleaseInstance::Request& input,
            MaterialCommands::ReleaseInstance::Response& response
        ) noexcept
        {
            const auto released = resource.release(input.material);
            response.error = released ? RenderError{} : released.error();
        }

        void handleUnregisterProgram(
            MaterialStorage& resource, const MaterialCommands::UnregisterProgram::Request& input,
            MaterialCommands::UnregisterProgram::Response& response
        ) noexcept
        {
            const auto removed = resource.unregisterProgram(input.program);
            response.error = removed ? RenderError{} : removed.error();
        }
    }

    std::span<const CommandBinding> materialCommands() noexcept
    {
        static const std::array commands{
            CommandBinding::declare<MaterialCommands::RegisterProgram, &handleRegisterProgram>(),
            CommandBinding::declare<MaterialCommands::PrepareMeshPipelines, &prepareMeshPipelines>(),
            CommandBinding::declare<MaterialCommands::CreateInstance, &handleCreateInstance>(),
            CommandBinding::declare<MaterialCommands::Update, &updateMaterial>(),
            CommandBinding::declare<MaterialCommands::UpdateColor, &updateMaterialColors>(),
            CommandBinding::declare<MaterialCommands::ReleaseInstance, &releaseInstance>(),
            CommandBinding::declare<MaterialCommands::UnregisterProgram, &handleUnregisterProgram>()
        };
        return commands;
    }
}
