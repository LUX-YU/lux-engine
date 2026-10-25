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
        struct ShutdownWaiter
        {
            ShutdownWaiter* next = nullptr;
            std::shared_ptr<const void> code_lifetime;
            void (*complete)(ShutdownWaiter&) noexcept = nullptr;
        };

    public:
        class ShutdownSender final
        {
        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;

            template <class Receiver> struct Operation final : ShutdownWaiter
            {
                using operation_state_concept = stdexec::operation_state_t;
                RenderExecution& rendering;
                Receiver receiver;

                Operation(RenderExecution& rendering, Receiver receiver) noexcept
                    : rendering(rendering), receiver(std::move(receiver))
                {
                    this->code_lifetime = execution::detail::getCompletionCode(stdexec::get_env(this->receiver));
                    this->complete = [](ShutdownWaiter& waiter) noexcept
                    {
                        auto& operation = static_cast<Operation&>(waiter);
                        stdexec::set_value(std::move(operation.receiver));
                    };
                }

                void start() & noexcept
                {
                    rendering.awaitShutdown(*this);
                }
            };

            template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
            {
                return Operation<Receiver>{rendering_, std::move(receiver)};
            }

        private:
            friend class RenderExecution;

            explicit ShutdownSender(RenderExecution& rendering) noexcept : rendering_(rendering) {}

            RenderExecution& rendering_;
        };

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
        // Main-thread observation. Failure stops production, but does not stop completion/close progress.
        [[nodiscard]] render::RenderError failure() const noexcept;
        [[nodiscard]] render::RenderServerStats stats() const noexcept;

        // Nonblocking application close. Drive Main continuations and advanceClose until it reports completion.
        void beginClose() noexcept;
        [[nodiscard]] bool advanceClose() noexcept;

        // Main-thread completion after both protocol threads join. Stop does not waive physical cleanup.
        // The caller retains this RenderExecution and drives advanceClose; this sender never starts shutdown.
        [[nodiscard]] ShutdownSender whenStopped() noexcept
        {
            return ShutdownSender{*this};
        }

        // Synchronous standalone shutdown after business producers stop. Cancel unsent inputs, drain accepted replies,
        // close each Client on its SPSC thread, then stop/join Server. Unexpected Server failure retains
        // Unknown settlement. Main continuations and the shared CPU/IO execution remain alive throughout.
        [[nodiscard]] render::RenderResult<void> shutdown() noexcept;

    private:
        struct Impl;
        explicit RenderExecution(std::unique_ptr<Impl> impl) noexcept;
        void awaitShutdown(ShutdownWaiter&) noexcept;
        static void deliverShutdown(ShutdownWaiter&) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::rendering
