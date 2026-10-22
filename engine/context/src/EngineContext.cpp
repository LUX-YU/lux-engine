#include <lux/engine/EngineContext.hpp>

namespace lux::engine
{
    EngineContext::EngineContext(execution::ExecutionRuntime&& execution, ContextExtensions::Composition&& extensions)
        : execution_(std::move(execution)), extensions_(std::move(extensions).publish())
    {
    }

    EngineContext::~EngineContext() = default;

    EngineContext::CreateResult EngineContext::create(
        execution::ExecutionRuntimeConfig execution_config,
        std::size_t scene_task_capacity,
        ContextExtensions::Composition extensions
    ) noexcept
    {
        auto execution = execution::ExecutionRuntime::create(execution_config);
        if (!execution)
        {
            return lux::cxx::unexpected(VCreateFailure{execution.error()});
        }
        auto context = std::unique_ptr<EngineContext>(new EngineContext(std::move(*execution), std::move(extensions)));
        auto scenes = scene::SceneRuntime::create(context->execution_, scene_task_capacity);
        if (!scenes)
        {
            return lux::cxx::unexpected(VCreateFailure{scenes.error()});
        }
        context->scenes_ = std::move(*scenes);
        return context;
    }
} // namespace lux::engine
