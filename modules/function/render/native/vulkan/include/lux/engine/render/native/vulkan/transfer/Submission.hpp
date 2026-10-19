#pragma once

#include <lux/engine/render/native/vulkan/device/Device.hpp>
#include <memory>
#include <span>
#include <stdexec/execution.hpp>

namespace lux::render::vulkan
{
    class SubmissionQueue;
    using SubmissionCallbackFn = void (*)(void*, RenderResult<void>) noexcept;

    // Address-stable, non-owning completion node. Unlinked before invoking user code.
    class SubmissionCallback final
    {
    public:
        SubmissionCallback() noexcept = default;
        ~SubmissionCallback() noexcept;
        SubmissionCallback(const SubmissionCallback&) = delete;
        SubmissionCallback& operator=(const SubmissionCallback&) = delete;
        void reset() noexcept;

        [[nodiscard]] bool armed() const noexcept
        {
            return armed_;
        }

    private:
        friend class SubmissionQueue;
        SubmissionCallback* next_ = nullptr;
        SubmissionCallbackFn callback_ = nullptr;
        void* user_ = nullptr;
        std::uint64_t serial_ = 0;
        bool armed_ = false;
    };

    // Begin, record and submit run in one synchronous Owner execution segment.
    class CommandBatch final
    {
    public:
        ~CommandBatch() noexcept;
        CommandBatch(CommandBatch&& other) noexcept;
        CommandBatch& operator=(CommandBatch&&) = delete;
        CommandBatch(const CommandBatch&) = delete;
        CommandBatch& operator=(const CommandBatch&) = delete;

        [[nodiscard]] VkCommandBuffer native() const noexcept
        {
            return command_;
        }

        [[nodiscard]] const SubmissionQueue& owner() const noexcept
        {
            return *owner_;
        }

        [[nodiscard]] std::uint32_t slot() const noexcept
        {
            return slot_;
        }

        [[nodiscard]] std::uint64_t recordingId() const noexcept
        {
            return recording_id_;
        }

        [[nodiscard]] RenderResult<void> submit() && noexcept;

        [[nodiscard]] RenderResult<void> submit(
            SubmissionCallback& completion,
            SubmissionCallbackFn function,
            void* user
        ) && noexcept;

        [[nodiscard]] RenderResult<void> submitAfter(
            const SubmissionQueue& source,
            VkPipelineStageFlags2 stages
        ) && noexcept;

        [[nodiscard]] RenderResult<void> submit(VkSemaphore acquired, VkSemaphore present) && noexcept;

    private:
        friend class SubmissionQueue;
        CommandBatch(SubmissionQueue&, VkCommandBuffer, std::uint32_t slot, std::uint64_t recording) noexcept;
        SubmissionQueue* owner_;
        VkCommandBuffer command_;
        std::uint32_t slot_;
        std::uint64_t recording_id_;
    };

    // One physical queue and its slots. Submission identity stays in Native progress/retirement state.
    class SubmissionQueue final
    {
    public:
        class CompletionSender;

        [[nodiscard]] static RenderResult<std::unique_ptr<SubmissionQueue>> create(
            VkDevice device,
            NativeQueue native_queue,
            bool timeline_enabled,
            std::uint32_t capacity
        ) noexcept;
        ~SubmissionQueue() noexcept;
        SubmissionQueue(const SubmissionQueue&) = delete;
        SubmissionQueue& operator=(const SubmissionQueue&) = delete;

        [[nodiscard]] RenderResult<CommandBatch> begin() noexcept;
        [[nodiscard]] RenderResult<void> poll() noexcept;
        [[nodiscard]] RenderResult<bool> waitIdle(std::uint64_t timeout_ns) noexcept;
        [[nodiscard]] CompletionSender whenIdle() noexcept;
        [[nodiscard]] CompletionSender whenAvailable() noexcept;
        // Only this explicit Owner safe point invokes callbacks, never poll/begin/wait.
        void dispatchCompleted() noexcept;
        [[nodiscard]] std::uint32_t capacity() const noexcept;
        [[nodiscard]] VkDevice device() const noexcept;
        [[nodiscard]] bool recording() const noexcept;
        [[nodiscard]] bool idle() const noexcept;
        [[nodiscard]] bool deviceLost() const noexcept;
        [[nodiscard]] NativeQueue nativeQueue() const noexcept;

    private:
        friend class CommandBatch;
        friend class ExecutionContext;
        friend class SubmissionCallback;
        struct Backing;
        explicit SubmissionQueue(std::unique_ptr<Backing>) noexcept;
        [[nodiscard]] RenderResult<CommandBatch> beginAfterPoll() noexcept;
        [[nodiscard]] RenderResult<void> submit(
            std::span<const VkSemaphoreSubmitInfo> waits = {},
            VkSemaphore acquired = {},
            VkSemaphore present = {},
            SubmissionCallback* completion = nullptr,
            SubmissionCallbackFn function = nullptr,
            void* user = nullptr
        ) noexcept;
        [[nodiscard]] RenderResult<VkSemaphoreSubmitInfo> dependency(VkPipelineStageFlags2 stages) const noexcept;
        [[nodiscard]] RenderResult<bool> wait(std::uint64_t serial, std::uint64_t timeout_ns) noexcept;
        [[nodiscard]] std::uint64_t completed() const noexcept;
        [[nodiscard]] std::uint64_t submitted() const noexcept;
        void arm(std::uint64_t serial, SubmissionCallback&, SubmissionCallbackFn, void*) noexcept;
        void cancel() noexcept;
        void completePending() noexcept;
        RenderError failure(VkResult) noexcept;
        std::unique_ptr<Backing> backing_;
    };

    class SubmissionQueue::CompletionSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(RenderResult<void>)>;

        template <class Receiver> class Operation final
        {
        public:
            using operation_state_concept = stdexec::operation_state_t;

            Operation(SubmissionQueue& queue, std::uint64_t serial, Receiver receiver) noexcept
                : queue_(queue), serial_(serial), receiver_(std::move(receiver))
            {
            }

            void start() & noexcept
            {
                queue_.arm(serial_, callback_, finish, this);
            }

        private:
            static void finish(void* value, RenderResult<void> result) noexcept
            {
                auto& self = *static_cast<Operation*>(value);
                self.callback_.reset();
                stdexec::set_value(std::move(self.receiver_), std::move(result));
            }

            SubmissionQueue& queue_;
            std::uint64_t serial_;
            SubmissionCallback callback_;
            Receiver receiver_;
        };

        template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
        {
            return Operation<Receiver>{queue_, serial_, std::move(receiver)};
        }

    private:
        friend class SubmissionQueue;

        CompletionSender(SubmissionQueue& queue, std::uint64_t serial) noexcept : queue_(queue), serial_(serial) {}

        SubmissionQueue& queue_;
        std::uint64_t serial_;
    };
} // namespace lux::render::vulkan
