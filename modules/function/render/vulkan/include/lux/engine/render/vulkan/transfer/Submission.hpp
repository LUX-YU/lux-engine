#pragma once

#include <memory>
#include <lux/engine/render/vulkan/device/Device.hpp>

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

        [[nodiscard]] const SubmissionQueue &owner() const noexcept
        {
            return *owner_;
        }

    private:
        friend class SubmissionQueue;

        SubmissionTicket(const SubmissionQueue &owner, std::uint64_t serial) noexcept : owner_(&owner), serial_(serial)
        {
        }

        const SubmissionQueue *owner_;
        std::uint64_t serial_;
    };

    class CommandBatch
    {
    public:
        ~CommandBatch() noexcept;
        CommandBatch(CommandBatch &&other) noexcept;
        CommandBatch &operator=(CommandBatch &&) = delete;
        CommandBatch(const CommandBatch &) = delete;
        CommandBatch &operator=(const CommandBatch &) = delete;

        [[nodiscard]] VkCommandBuffer native() const noexcept
        {
            return command_;
        }

        [[nodiscard]] const SubmissionQueue &owner() const noexcept
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

    private:
        friend class SubmissionQueue;
        CommandBatch(
            SubmissionQueue &owner, VkCommandBuffer command, std::uint32_t slot, std::uint64_t recording
        ) noexcept;
        SubmissionQueue *owner_;
        VkCommandBuffer command_;
        std::uint32_t slot_;
        std::uint64_t recording_id_;
    };

    // One externally serialized owner domain; no thread, frame loop or transport.
    // The owning unique_ptr can move, while this address stays stable for borrows.
    class SubmissionQueue
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<SubmissionQueue>> create(
            const VulkanDevice &device, std::uint32_t capacity
        ) noexcept;
        ~SubmissionQueue() noexcept;
        SubmissionQueue(const SubmissionQueue &) = delete;
        SubmissionQueue &operator=(const SubmissionQueue &) = delete;

        [[nodiscard]] RenderResult<CommandBatch> begin() noexcept;
        [[nodiscard]] RenderResult<std::uint64_t> poll() noexcept;
        [[nodiscard]] RenderResult<bool> wait(SubmissionTicket ticket, std::uint64_t timeout_ns) noexcept;
        [[nodiscard]] bool owns(SubmissionTicket ticket) const noexcept;
        [[nodiscard]] std::uint64_t completed() const noexcept;
        [[nodiscard]] std::uint64_t submitted() const noexcept;
        [[nodiscard]] std::uint32_t capacity() const noexcept;
        [[nodiscard]] VkDevice device() const noexcept;
        [[nodiscard]] bool recording() const noexcept;
        [[nodiscard]] bool deviceLost() const noexcept;

    private:
        friend class CommandBatch;
        friend class StagingArena;
        friend class RetirementQueue;
        struct Backing;
        explicit SubmissionQueue(std::unique_ptr<Backing> backing) noexcept;
        [[nodiscard]] RenderResult<SubmissionTicket> submit() noexcept;
        void cancel() noexcept;
        // Final owner teardown only; never used by ordinary backing destruction.
        void completePending() noexcept;
        RenderError failure(VkResult result) noexcept;

        std::unique_ptr<Backing> backing_;
    };
} // namespace lux::render::vulkan
