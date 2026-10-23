#pragma once

#include <lux/engine/core/execution/RunLoop.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/rendering/RenderRequester.hpp>

namespace lux::execution
{
    class ExecutionRuntime;
}

namespace lux::rendering
{
    struct RenderExecutionConfig final
    {
        render::RenderServerConfig render;
        render::DeviceOptions device;
        std::size_t requests_per_client = 1024;
        // Optional cold registration, before either protocol thread starts.
        void* feature_registration = nullptr;
        render::RenderResult<void> (*register_features)(void*, render::RenderServer&) noexcept = nullptr;
    };

    // Application rendering facilities: one Server, one RenderThread and one fixed Asset protocol thread.
    // CPU work uses the borrowed ExecutionRuntime. This class owns no asset or Scene business directory.
    class RenderExecution final
    {
    public:
        using CreateResult = render::RenderResult<std::unique_ptr<RenderExecution>>;

        // Synchronous startup on Main, including command discovery. Installs execution.wake() as the
        // Server's reply-publication notification; window wakeup belongs to ExecutionRuntime::setWake().
        [[nodiscard]] static CreateResult create(
            RenderExecutionConfig config,
            execution::ExecutionRuntime& execution
        ) noexcept;

        // shutdown() must have joined both protocol threads. Destruction never pumps application callbacks.
        ~RenderExecution() noexcept;
        RenderExecution(const RenderExecution&) = delete;
        RenderExecution& operator=(const RenderExecution&) = delete;

        [[nodiscard]] execution::ExecutionRuntime& execution() const noexcept;
        [[nodiscard]] RenderRequester& mainClient() noexcept;
        // Cold binding may read its immutable catalog after create(). Start requests and consume
        // responses only after switching to assetScheduler(), on that fixed SPSC thread.
        [[nodiscard]] RenderRequester& assetClient() noexcept;
        [[nodiscard]] execution::RunLoop::Scheduler assetScheduler() noexcept;

        // Main safe point. Consumes replies and joins an already terminated Server before settling
        // missing responses. This also lets Asset pipelines finish after an unexpected Server exit.
        std::size_t consumeResponses() noexcept;
        [[nodiscard]] render::RenderServerStats stats() const noexcept;

        // Main-thread shutdown after all Asset pipelines/producers have settled and stopped publishing.
        // Drains Server work, joins it, settles missing replies as Unknown, then joins the Asset thread.
        // Main continuations remain driven while Server stops; the shared CPU/IO execution stays alive.
        [[nodiscard]] render::RenderResult<void> shutdown() noexcept;

    private:
        struct Impl;
        explicit RenderExecution(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::rendering
