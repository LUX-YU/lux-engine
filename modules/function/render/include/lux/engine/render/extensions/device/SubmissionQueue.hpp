#pragma once

#include <NRI.h>
#include <array>
#include <exec/task.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/visibility.h>
#include <memory>
#include <optional>
#include <span>
#include <stdexec/execution.hpp>
#include <vector>

namespace lux::render::detail
{
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

        [[nodiscard]] static RenderResult<std::unique_ptr<SubmissionQueue>>
        create(DeviceSession&, nri::Queue&, QueueProperties) noexcept;
        ~SubmissionQueue() noexcept;
        SubmissionQueue(const SubmissionQueue&) = delete;
        SubmissionQueue& operator=(const SubmissionQueue&) = delete;

        // Start on the single control owner. The operation freezes descriptor arrays, borrows the
        // command payloads until its native return, and publishes a serial only after real acceptance.
        [[nodiscard]] exec::task<RenderResult<std::uint64_t>> submit(
            std::span<nri::CommandBuffer* const> commands,
            std::span<const nri::FenceSubmitDesc> waits = {},
            std::span<const nri::FenceSubmitDesc> signals = {}
        ) noexcept;

        [[nodiscard]] bool submitting() const noexcept
        {
            return submitting_;
        }

        [[nodiscard]] RenderError terminalError() const noexcept
        {
            return terminal_.value_or(RenderError{});
        }

        void dispatchCompleted() noexcept;
        [[nodiscard]] RenderResult<void> poll() noexcept;
        [[nodiscard]] RenderResult<bool> waitIdle(std::uint64_t timeout_ns) noexcept;
        [[nodiscard]] CompletionSender whenSubmittedWorkCompletes() noexcept;
        // An accepted frontier captured by the submitting operation; excludes later unrelated submissions.
        [[nodiscard]] CompletionSender whenSubmissionCompletes(std::uint64_t serial) noexcept;
        [[nodiscard]] bool idle() const noexcept;
        [[nodiscard]] bool deviceLost() const noexcept;
        [[nodiscard]] nri::Queue& native() const noexcept;
        [[nodiscard]] const QueueProperties& properties() const noexcept;

    private:
        friend class SubmissionCallback;
        SubmissionQueue(DeviceSession&, nri::Queue&, QueueProperties) noexcept;

    public:
        [[nodiscard]] RenderResult<nri::FenceSubmitDesc> dependency(std::uint64_t serial, nri::StageBits)
            const noexcept;
        [[nodiscard]] RenderResult<bool> wait(std::uint64_t serial, std::uint64_t timeout_ns) noexcept;
        [[nodiscard]] std::uint64_t completed() const noexcept;
        [[nodiscard]] std::uint64_t submitted() const noexcept;
        void arm(std::uint64_t serial, SubmissionCallback&, SubmissionCallbackFn, void*) noexcept;

    private:
        RenderError failure(nri::Result) noexcept;

        DeviceSession& session_;
        nri::Queue& queue_;
        QueueProperties properties_;
        nri::Fence* fence_ = nullptr;
        std::uint64_t submitted_ = 0, completed_ = 0;
        bool submitting_ = false;
        bool terminal_retired_ = false;
        std::optional<RenderError> terminal_;
        // Borrowed by the native call while the owner holds submitting_ across delivery.
        struct SubmissionStorage final
        {
            std::vector<nri::CommandBuffer*> commands;
            std::vector<nri::FenceSubmitDesc> waits;
            std::vector<nri::FenceSubmitDesc> signals;
        };

        SubmissionStorage submission_storage_;
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
