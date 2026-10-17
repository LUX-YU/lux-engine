#pragma once

#include <lux/engine/render/native/vulkan/device/Device.hpp>
#include <array>
#include <memory>
#include <optional>
#include <span>

namespace lux::render::vulkan
{
    class SubmissionQueue;

    // Borrowed queue-scoped evidence of successful native admission, not completion.
    // Tokens cannot outlive their queue; serials from different queues never compare.
    class SubmissionTicket
    {
    public:
        [[nodiscard]] std::uint64_t serial() const noexcept
        {
            return serial_;
        }

        [[nodiscard]] const SubmissionQueue& owner() const noexcept
        {
            return *owner_;
        }

    private:
        friend class SubmissionQueue;

        SubmissionTicket(const SubmissionQueue& owner, std::uint64_t serial) noexcept : owner_(&owner), serial_(serial)
        {
        }

        const SubmissionQueue* owner_;
        std::uint64_t serial_;
    };

    // Stable backing-owned conditions. Roles sharing one physical queue merge into the same entry.
    // Like SubmissionTicket, this value borrows queues and cannot outlive their DeviceSession.
    class SubmissionRequirements final
    {
    public:
        struct Use final { const SubmissionQueue* queue = nullptr; std::uint64_t serial = 0; };

        void merge(SubmissionTicket ticket) noexcept;
        [[nodiscard]] bool completed() const noexcept;
        [[nodiscard]] bool empty() const noexcept { return uses_[0].queue == nullptr; }
        [[nodiscard]] std::span<const Use> uses() const noexcept { return uses_; }

    private:
        std::array<Use, 3> uses_{};
    };

    using SubmissionCallbackFn = void (*)(void*, RenderResult<void>) noexcept;

    // Embedded in an address-stable operation. Reservation survives dispatch and can fund another chunk.
    // An armed node must live through its callback; reset/destruction only releases an unarmed reservation.
    class SubmissionCallback final
    {
    public:
        SubmissionCallback() noexcept = default;
        ~SubmissionCallback() noexcept;
        SubmissionCallback(const SubmissionCallback&) = delete;
        SubmissionCallback& operator=(const SubmissionCallback&) = delete;

        void reset() noexcept;
        [[nodiscard]] bool reserved() const noexcept { return queue_ != nullptr; }
        [[nodiscard]] bool armed() const noexcept { return armed_; }

    private:
        friend class SubmissionQueue;
        SubmissionQueue* queue_ = nullptr;
        SubmissionCallback* next_ = nullptr;
        SubmissionCallbackFn callback_ = nullptr;
        void* user_ = nullptr;
        std::uint64_t serial_ = 0;
        bool armed_ = false;
    };

    class CommandBatch
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

        [[nodiscard]] RenderResult<SubmissionTicket> submit() && noexcept;
        [[nodiscard]] RenderResult<SubmissionTicket> submit(std::span<const struct SubmissionWait> waits) && noexcept;
        // Synchronous native WSI borrows. Caller retains semaphores until actual
        // acquire consumption / Present completion, independently of this ticket.
        [[nodiscard]] RenderResult<SubmissionTicket> submit(
            std::span<const struct SubmissionWait> waits,
            VkSemaphore acquired,
            VkSemaphore present
        ) && noexcept;

    private:
        friend class SubmissionQueue;
        CommandBatch(
            SubmissionQueue& owner,
            VkCommandBuffer command,
            std::uint32_t slot,
            std::uint64_t recording
        ) noexcept;
        SubmissionQueue* owner_;
        VkCommandBuffer command_;
        std::uint32_t slot_;
        std::uint64_t recording_id_;
    };

    struct SubmissionWait
    {
        SubmissionTicket ticket;
        VkPipelineStageFlags2 stages;
    };

    // One externally serialized owner domain; no thread, frame loop or transport.
    // The owning unique_ptr can move, while this address stays stable for borrows.
    class SubmissionQueue
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<SubmissionQueue>> create(
            const VulkanDevice& device,
            std::uint32_t capacity,
            EQueueRole role = EQueueRole::GRAPHICS,
            std::uint32_t callback_capacity = 128
        ) noexcept;
        ~SubmissionQueue() noexcept;
        SubmissionQueue(const SubmissionQueue&) = delete;
        SubmissionQueue& operator=(const SubmissionQueue&) = delete;

        [[nodiscard]] RenderResult<CommandBatch> begin() noexcept;
        [[nodiscard]] RenderResult<std::uint64_t> poll() noexcept;
        [[nodiscard]] RenderResult<bool> wait(SubmissionTicket ticket, std::uint64_t timeout_ns) noexcept;
        [[nodiscard]] bool owns(SubmissionTicket ticket) const noexcept;
        [[nodiscard]] std::uint64_t completed() const noexcept;
        [[nodiscard]] std::uint64_t submitted() const noexcept;

        // Reserve before Submit; arming an accepted Ticket has no recoverable capacity failure.
        [[nodiscard]] RenderResult<void> reserve(SubmissionCallback& callback) noexcept;
        [[nodiscard]] RenderResult<void> onComplete(SubmissionTicket ticket, SubmissionCallback& callback,
            SubmissionCallbackFn function, void* user) noexcept;
        // Explicit owner-thread safe point. poll/begin/wait never invoke business callbacks.
        // Re-registered nodes, including already completed Tickets, wait for the next dispatch.
        void dispatchCompleted() noexcept;
        [[nodiscard]] std::size_t callbackReservations() const noexcept;
        [[nodiscard]] std::size_t callbackCapacity() const noexcept;
        [[nodiscard]] std::optional<SubmissionTicket> oldestPending() const noexcept
        {
            return completed() < submitted() ? std::optional{SubmissionTicket{*this, completed() + 1}} : std::nullopt;
        }

        // Actual admitted work, including a partial Graph submission. No synthetic frame ticket.
        [[nodiscard]] std::optional<SubmissionTicket> lastSubmitted() const noexcept
        {
            const auto serial = submitted();
            return serial ? std::optional{SubmissionTicket{*this, serial}} : std::nullopt;
        }

        [[nodiscard]] std::uint32_t capacity() const noexcept;
        [[nodiscard]] VkDevice device() const noexcept;
        [[nodiscard]] bool recording() const noexcept;
        [[nodiscard]] bool deviceLost() const noexcept;
        [[nodiscard]] NativeQueue nativeQueue() const noexcept;

    private:
        friend class CommandBatch;
        friend class ExecutionContext;
        // Same serialized owner has just checked recording and polled; no callback/Submit in between.
        [[nodiscard]] RenderResult<CommandBatch> beginAfterPoll() noexcept;
        friend class SubmissionCallback;
        friend class StagingArena;
        friend class RetirementQueue;
        struct Backing;
        explicit SubmissionQueue(std::unique_ptr<Backing> backing) noexcept;
        [[nodiscard]] RenderResult<SubmissionTicket> submit(
            std::span<const SubmissionWait> waits,
            VkSemaphore acquired = {},
            VkSemaphore present = {}
        ) noexcept;
        void cancel() noexcept;
        void releaseCallback(SubmissionCallback& callback) noexcept;
        // Final owner teardown only; never used by ordinary backing destruction.
        void completePending() noexcept;
        RenderError failure(VkResult result) noexcept;

        std::unique_ptr<Backing> backing_;
    };
} // namespace lux::render::vulkan
