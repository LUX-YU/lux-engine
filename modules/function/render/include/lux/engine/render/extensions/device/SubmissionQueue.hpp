#pragma once

#include <NRI.h>
#include <array>
#include <functional>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/visibility.h>
#include <memory>
#include <optional>
#include <span>
#include <stdexec/execution.hpp>
#include <vector>

namespace lux::render::detail
{
    struct SubmissionFrontiers final
    {
        std::array<std::uint64_t, 3> serials{};
    };

    class DeviceSession;
    class SubmissionQueue;
    using SubmissionCallbackFn = void (*)(void*, RenderResult<void>) noexcept;

    enum class EQueueRole : std::uint8_t
    {
        GRAPHICS,
        COMPUTE,
        TRANSFER
    };

    // Cold physical queue facts needed by uploads and timestamp interpretation.
    struct QueueProperties final
    {
        nri::QueueType type = nri::QueueType::GRAPHICS;
        std::uint32_t family = 0;
        std::array<std::uint32_t, 3> image_transfer_granularity{1, 1, 1};
        std::uint32_t timestamp_bits = 0;
    };

    // Address-stable, non-owning completion node. Unlinked before invoking user code.
    class LUX_RENDER_PUBLIC SubmissionCallback final
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

    class LUX_RENDER_PUBLIC SubmissionQueue final
    {
    public:
        class CompletionSender;

        [[nodiscard]] static RenderResult<std::unique_ptr<SubmissionQueue>> create(
            DeviceSession&,
            nri::Queue&,
            QueueProperties,
            std::uint32_t capacity
        ) noexcept;
        ~SubmissionQueue() noexcept;
        SubmissionQueue(const SubmissionQueue&) = delete;
        SubmissionQueue& operator=(const SubmissionQueue&) = delete;

        // The queue keeps recording and submission in one non-suspending device-thread segment.
        // Record must balance its rendering scopes even when it returns a recoverable error.
        template <class Record>
        [[nodiscard]] RenderResult<void> recordAndSubmit(
            Record&& record,
            std::span<const nri::FenceSubmitDesc> waits = {},
            std::span<const nri::FenceSubmitDesc> signals = {}
        ) noexcept
        {
            static_assert(std::is_nothrow_invocable_r_v<
                          RenderResult<void>,
                          Record&,
                          nri::CommandBuffer&,
                          std::uint32_t>);
            std::uint32_t slot = 0;
            auto commands = begin(slot);
            if (!commands)
            {
                return cxx::unexpected(commands.error());
            }
            cxx::scope_exit release([this]() noexcept { cancelRecording(); });
            auto recorded = std::invoke(record, **commands, slot);
            auto ended = finishRecording();
            if (!ended)
            {
                return ended;
            }
            if (!recorded)
            {
                return recorded;
            }
            return submit(waits, signals);
        }

        [[nodiscard]] RenderResult<void> poll() noexcept;
        [[nodiscard]] RenderResult<bool> waitIdle(std::uint64_t timeout_ns) noexcept;
        [[nodiscard]] CompletionSender whenSubmittedWorkCompletes() noexcept;
        // An accepted frontier captured by FrameExecution; excludes later unrelated submissions.
        [[nodiscard]] CompletionSender whenSubmissionCompletes(std::uint64_t serial) noexcept;
        [[nodiscard]] CompletionSender whenAvailable() noexcept;
        // Detach the ready prefix before invoking callbacks; a callback may destroy its own node.
        void dispatchCompleted() noexcept;
        [[nodiscard]] std::uint32_t capacity() const noexcept;
        // Cached frontier only; preparing a frame never polls or dispatches callbacks.
        [[nodiscard]] bool hasCapacity() const noexcept;
        [[nodiscard]] bool recording() const noexcept;
        [[nodiscard]] bool idle() const noexcept;
        [[nodiscard]] bool deviceLost() const noexcept;
        [[nodiscard]] nri::Queue& native() const noexcept;
        [[nodiscard]] const QueueProperties& properties() const noexcept;

    private:
        friend class SubmissionCallback;
        SubmissionQueue(DeviceSession&, nri::Queue&, QueueProperties, std::uint32_t capacity) noexcept;
        [[nodiscard]] RenderResult<nri::CommandBuffer*> begin(std::uint32_t& slot) noexcept;
        [[nodiscard]] RenderResult<void> finishRecording() noexcept;
        [[nodiscard]] RenderResult<void> submit(
            std::span<const nri::FenceSubmitDesc> waits,
            std::span<const nri::FenceSubmitDesc> signals
        ) noexcept;

    public:
        [[nodiscard]] RenderResult<nri::FenceSubmitDesc> dependency(std::uint64_t serial, nri::StageBits)
            const noexcept;
        [[nodiscard]] RenderResult<bool> wait(std::uint64_t serial, std::uint64_t timeout_ns) noexcept;
        [[nodiscard]] std::uint64_t completed() const noexcept;
        [[nodiscard]] std::uint64_t submitted() const noexcept;
        void arm(std::uint64_t serial, SubmissionCallback&, SubmissionCallbackFn, void*) noexcept;

    private:
        void cancelRecording() noexcept;
        void completePending() noexcept;
        RenderError failure(nri::Result) noexcept;

        struct SubmittedBatch final
        {
            nri::CommandAllocator* allocator = nullptr;
            nri::CommandBuffer* command = nullptr;
        };

        DeviceSession& session_;
        nri::Queue& queue_;
        QueueProperties properties_;
        nri::Fence* fence_ = nullptr;
        std::vector<SubmittedBatch> batches_;
        std::vector<nri::FenceSubmitDesc> submission_signals_;
        std::uint64_t submitted_ = 0, completed_ = 0;
        bool recording_ = false;
        std::optional<RenderError> terminal_;
        SubmissionCallback* callbacks_ = nullptr;
        SubmissionCallback* callbacks_tail_ = nullptr;
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
} // namespace lux::render::detail
