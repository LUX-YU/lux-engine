#include <lux/engine/render/assembly/BuiltinEffects.hpp>
#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <lux/engine/render/runtime/commands/TransformCommands.hpp>
#include <lux/engine/render/resource/transform/TransformStorage.hpp>
#include <utility>

namespace lux::render::detail
{
    namespace
    {
        void createTransforms(
            TransformStorage& storage, const TransformCommands::Create::Request& input,
            TransformCommands::Create::Response& reply
        ) noexcept
        {
            auto result = storage.create(input.matrices);
            reply = result ? TransformCommands::Create::Response{{}, std::move(*result)}
                           : TransformCommands::Create::Response{result.error()};
        }

        void updateTransformsCommand(
            TransformStorage& storage, const TransformCommands::Update::Request& input, AckResponse& reply
        ) noexcept
        {
            const auto result = storage.update(input.values);
            reply.error = result ? RenderError{} : result.error();
        }

        void releaseTransform(TransformStorage& storage, const TransformRef& id, AckResponse& reply) noexcept
        {
            const auto result = storage.release(id);
            reply.error = result ? RenderError{} : result.error();
        }
    }

    std::span<const CommandBinding> transformStorageCommands() noexcept
    {
        static const std::array commands{
            CommandBinding::declare<TransformCommands::Create, &createTransforms>(),
            CommandBinding::declare<TransformCommands::Update, &updateTransformsCommand>(),
            CommandBinding::declare<TransformCommands::Release, &releaseTransform>()
        };
        return commands;
    }
}
