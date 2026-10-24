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
        std::size_t asset_pending_requests = 1024; // Independent streaming admission, never a Main frame limit.
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

        // Main frame entry: nullptr means wait for a previous frame before preparing new inputs.
        [[nodiscard]] render::RenderResult<FrameInput*> beginFrame() noexcept;
        // Takes over the open carrier. Report failed completion, then recycle it after all replies.
        // The application must not use/release this carrier after calling submitFrame, even on failure.
        [[nodiscard]] render::RenderResult<void> submitFrame(FrameInput& frame) noexcept;

        // Main safe point: consume replies and report an actual Server failure here, not by polling Link state.
        // An unexpected exit is joined before missing responses release their borrowed inputs.
        render::RenderResult<std::size_t> consumeResponses() noexcept;
        [[nodiscard]] render::RenderServerStats stats() const noexcept;

        // Nonblocking application close. Drive Main continuations and advanceClose until it reports completion.
        void beginClose() noexcept;
        [[nodiscard]] render::RenderResult<bool> advanceClose() noexcept;

        // Synchronous standalone shutdown after business producers stop. Cancel unsent inputs, drain accepted replies,
        // close each Client on its SPSC thread, then stop/join Server. Unexpected Server failure retains
        // Unknown settlement. Main continuations and the shared CPU/IO execution remain alive throughout.
        [[nodiscard]] render::RenderResult<void> shutdown() noexcept;

    private:
        struct Impl;
        explicit RenderExecution(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::rendering
