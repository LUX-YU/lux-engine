#include <cstdlib>
#include <lux/engine/Engine.hpp>
#include <lux/engine/detail/EngineContextImpl.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/log/Log.hpp>
#include <utility>

#if defined(LUX_ENGINE_WITH_RENDER)
#include <lux/engine/systems/render/RenderDiagnostics.hpp>
#endif

namespace lux::engine
{
    EngineContext::Impl::Impl(std::unique_ptr<execution::ExecutionRuntime> execution) noexcept
        : execution(std::move(execution))
    {
    }

    EngineContext::Impl::~Impl() noexcept
    {
#if defined(LUX_ENGINE_WITH_RENDER)
        frames.reset();
#endif
        assets.reset(); // Repository owns its task settlement and resource release.
#if defined(LUX_ENGINE_WITH_RENDER)
        if (render_thread.joinable())
        {
            stopping_render = true;
            server->stop();
            if (!execution->waitUntil([this]() noexcept { return !render_thread.joinable(); }))
            {
                std::terminate(); // Destruction is forbidden inside an Execution dispatch callback.
            }
        }
#endif
    }

#if defined(LUX_ENGINE_WITH_RENDER)
    void EngineContext::Impl::notifyResponses(void* context) noexcept
    {
        static_cast<Impl*>(context)->render_responses->request();
    }

    void EngineContext::Impl::dispatchRenderResponses() noexcept
    {
        constexpr std::size_t batch_size = 128;
        auto delivered = render_client->dispatchResponses(batch_size);
        const bool expected_closed = !delivered && stopping_render &&
            delivered.error().type == lux::render::kOutcomeUnknown;
        if (!delivered && !expected_closed)
        {
            render::reportRenderFailure(delivered.error(), "RenderClient response delivery failed");
            server->stop(); // A transport fault follows the existing unexpected-service-exit policy.
        }
        else if (delivered && *delivered == batch_size)
        {
            render_responses->request();
        }
    }

    void EngineContext::Impl::runRender() noexcept
    {
        auto result = server->run();
        auto complete = stdexec::then(
            stdexec::just(std::move(result)),
            [this](auto outcome) noexcept
            {
                render_thread.join();
                if (!stopping_render || !outcome)
                {
                    render::reportRenderFailure(
                        outcome ? error::Error{lux::render::kOutcomeUnknown} : outcome.error(),
                        "RenderServer terminated outside successful shutdown"
                    );
                    std::abort();
                }
                if (render_client) render_client->serverStopped({}); // Bootstrap failure has no published Client.
                render_responses->cancel();
            }
        );
        const auto accepted = execution->spawn(stdexec::starts_on(execution->mainScheduler(), std::move(complete)));
        if (!accepted)
        {
            std::terminate(); // Execution admission outlives this joined producer.
        }
    }
#endif

    EngineContext::EngineContext(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    EngineContext::~EngineContext() noexcept = default;

    execution::ExecutionRuntime& EngineContext::execution() noexcept
    {
        return *impl_->execution;
    }

    object::ObjectRuntime& EngineContext::objects() noexcept
    {
        return *impl_->objects;
    }

    asset::AssetVfs& EngineContext::assetVfs() noexcept
    {
        return impl_->asset_vfs;
    }

    assets::AssetRepository& EngineContext::assets() noexcept
    {
        return *impl_->assets;
    }

#if defined(LUX_ENGINE_WITH_RENDER)
    lux::render::RenderClient& EngineContext::renderClient() noexcept
    {
        return *impl_->render_client;
    }
#endif

    void EngineContext::bindEngineStages(Engine& engine) noexcept
    {
#if defined(LUX_ENGINE_WITH_RENDER)
        engine.prependEngineStageCallback(
            [this]() noexcept { return impl_->frames->beginFrame(); },
            EEngineStage::BEGIN_UPDATE
        );
        engine.addEngineStageCallback(
            [this]() noexcept { return impl_->frames->submitFrame(); },
            EEngineStage::END_UPDATE
        );
        engine.addEngineStageCallback(
            [this]() noexcept -> Engine::Result
            {
                impl_->frames->discardFrame();
                return {};
            },
            EEngineStage::UPDATE_ABORTED
        );
#else
        static_cast<void>(engine);
#endif
    }
} // namespace lux::engine
