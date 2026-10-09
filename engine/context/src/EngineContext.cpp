#include <lux/engine/EngineContext.hpp>

namespace lux::engine
{
    EngineContext::EngineContext(process::ExecutionRuntime&& execution, ContextExtensions::Composition&& extensions)
        : execution_(std::move(execution)), extensions_(std::move(extensions).publish())
    {
    }

    EngineContext::~EngineContext() = default;

    EngineContext::CreateResult EngineContext::create(
        process::ExecutionRuntimeConfig config,
        task::TaskExecutorConfig scene_executor,
        ContextExtensions::Composition extensions
    ) noexcept
    {
        auto execution = process::ExecutionRuntime::create(config);
        if (!execution)
        {
            return lux::cxx::unexpected(VCreateFailure{execution.error()});
        }
        auto context = std::unique_ptr<EngineContext>(new EngineContext(std::move(*execution), std::move(extensions)));
        auto scenes = scene::SceneRuntime::create(context->execution_, scene_executor);
        if (!scenes)
        {
            return lux::cxx::unexpected(VCreateFailure{scenes.error()});
        }
        context->scenes_ = std::move(*scenes);
        return context;
    }
} // namespace lux::engine
