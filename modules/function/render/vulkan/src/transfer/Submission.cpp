#include <lux/engine/render/vulkan/transfer/Submission.hpp>

#include <exception>
#include <limits>
#include <optional>
#include <utility>
#include <vector>
#include "Native.hpp"

namespace lux::render::vulkan
{
    struct SubmissionQueue::Backing
    {
        VkDevice device;
        NativeQueue queue;
        VkSemaphore timeline{};
        VkCommandPool pool{};
        std::vector<VkCommandBuffer> commands;
        std::vector<VkFence> fences;
        std::uint64_t submitted{};
        std::uint64_t completed{};
        std::uint64_t recording_id{};
        bool recording{};
        std::optional<RenderError> terminal;

        Backing(VkDevice device, NativeQueue queue, std::uint32_t capacity)
            : device(device), queue(queue), commands(capacity), fences(capacity)
        {
        }

        ~Backing() noexcept
        {
            if (timeline)
            {
                LUX_DESTROY("timeline", vkDestroySemaphore(device, timeline, nullptr));
            }
            for (auto fence : fences)
            {
                if (fence)
                    LUX_DESTROY("fence", vkDestroyFence(device, fence, nullptr));
            }
            if (pool)
                LUX_DESTROY("command_pool", vkDestroyCommandPool(device, pool, nullptr));
        }
    };

    SubmissionQueue::SubmissionQueue(std::unique_ptr<Backing> backing) noexcept : backing_(std::move(backing)) {}

    RenderResult<std::unique_ptr<SubmissionQueue>> SubmissionQueue::create(
        const VulkanDevice& device,
        std::uint32_t capacity,
        EQueueRole role
    ) noexcept
    {
        if (capacity == 0)
            return cxx::unexpected(RenderError{kInvalidArgument});
        if (static_cast<std::uint32_t>(role) > static_cast<std::uint32_t>(EQueueRole::TRANSFER))
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        auto backing = std::make_unique<Backing>(device.native(), device.queue(role), capacity);
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = backing->queue.family;
        auto result = LUX_NATIVE("command_pool", vkCreateCommandPool(device.native(), &pool, nullptr, &backing->pool));
        if (result != VK_SUCCESS)
            return cxx::unexpected(nativeError(result));
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = backing->pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = capacity;
        result =
            LUX_NATIVE("commands", vkAllocateCommandBuffers(device.native(), &allocation, backing->commands.data()));
        if (result != VK_SUCCESS)
            return cxx::unexpected(nativeError(result));
        const VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (auto &handle : backing->fences)
        {
            result = LUX_NATIVE("fence", vkCreateFence(device.native(), &fence, nullptr, &handle));
            if (result != VK_SUCCESS)
                return cxx::unexpected(nativeError(result));
        }
        if (device.timelineSemaphore())
        {
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            info.pNext = &type;
            result = LUX_NATIVE("timeline", vkCreateSemaphore(device.native(), &info, nullptr, &backing->timeline));
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
        }
        return std::unique_ptr<SubmissionQueue>{new SubmissionQueue(std::move(backing))};
    }

    SubmissionQueue::~SubmissionQueue() noexcept
    {
        completePending();
    }

    void SubmissionQueue::completePending() noexcept
    {
        // A live CommandBatch is a CPU borrow and must not outlive its owner.
        // Final teardown joins admitted GPU work by fence, without a public
        // shutdown protocol or vkDeviceWaitIdle on individual resources.
        if (backing_->recording)
            std::terminate();
        const bool has_pending = backing_->submitted != backing_->completed;
        if (!deviceLost() && has_pending)
        {
            auto result = wait(SubmissionTicket{*this, backing_->submitted}, UINT64_MAX);
            const bool cannot_teardown = !deviceLost() && (!result || !*result);
            if (cannot_teardown)
                std::terminate();
        }
    }

    RenderError SubmissionQueue::failure(VkResult result) noexcept
    {
        auto error = nativeError(result);
        if (result == VK_ERROR_DEVICE_LOST)
            backing_->terminal = error;
        return error;
    }

    RenderResult<CommandBatch> SubmissionQueue::begin() noexcept
    {
        if (backing_->terminal)
            return cxx::unexpected(*backing_->terminal);
        if (backing_->recording)
            return cxx::unexpected(RenderError{kBusy});
        auto completion = poll();
        if (!completion)
            return cxx::unexpected(completion.error());
        const bool is_full = backing_->submitted - backing_->completed == capacity();
        const bool is_exhausted = backing_->submitted == std::numeric_limits<std::uint64_t>::max() ||
                                  backing_->recording_id == std::numeric_limits<std::uint64_t>::max();
        if (is_full || is_exhausted)
            return cxx::unexpected(RenderError{kCapacity});
        const auto slot = static_cast<std::uint32_t>(backing_->submitted % capacity());
        const auto command = backing_->commands[slot];
        auto result = LUX_NATIVE("reset_command", vkResetCommandBuffer(command, 0));
        if (result != VK_SUCCESS)
            return cxx::unexpected(failure(result));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = LUX_NATIVE("begin", vkBeginCommandBuffer(command, &begin));
        if (result != VK_SUCCESS)
            return cxx::unexpected(failure(result));
        backing_->recording = true;
        ++backing_->recording_id;
        return CommandBatch{*this, command, slot, backing_->recording_id};
    }

    RenderResult<SubmissionTicket> SubmissionQueue::submit(std::span<const SubmissionWait> waits) noexcept
    {
        // Three actual queue roles; no per-submit allocation. Waits refer only
        // to already-admitted work, so this interface cannot create a future cycle.
        std::array<VkSemaphoreSubmitInfo, 3> native_waits{};
        if (waits.size() > native_waits.size())
        {
            return cxx::unexpected(RenderError{kCapacity});
        }
        for (std::size_t i = 0; i < waits.size(); ++i)
        {
            const auto& wait = waits[i];
            const auto& source = wait.ticket.owner();
            // Only stages from explicitly enabled Foundation features are admitted.
            // Unknown extension bits cannot silently become a valid queue wait.
            VkPipelineStageFlags2 allowed =
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
            const auto flags = nativeQueue().flags;
            if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0)
            {
                allowed |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT |
                           VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                           VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                           VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
                           VK_PIPELINE_STAGE_2_RESOLVE_BIT;
            }
            if ((flags & VK_QUEUE_COMPUTE_BIT) != 0)
            {
                allowed |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
            }
            const bool invalid = source.device() != device() || !source.owns(wait.ticket) || wait.stages == 0 ||
                                 (wait.stages & ~allowed) != 0;
            if (invalid)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            if (!source.backing_->timeline)
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (&waits[j].ticket.owner() == &source)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
            }
            native_waits[i] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            native_waits[i].semaphore = source.backing_->timeline;
            native_waits[i].value = wait.ticket.serial();
            native_waits[i].stageMask = wait.stages;
        }
        const auto slot = static_cast<std::uint32_t>(backing_->submitted % capacity());
        auto result = LUX_NATIVE("end", vkEndCommandBuffer(backing_->commands[slot]));
        if (result != VK_SUCCESS)
            return cxx::unexpected(failure(result));
        result = LUX_NATIVE("reset_fence", vkResetFences(backing_->device, 1, &backing_->fences[slot]));
        if (result != VK_SUCCESS)
            return cxx::unexpected(failure(result));
        VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        command.commandBuffer = backing_->commands[slot];
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = backing_->timeline;
        signal.value = backing_->submitted + 1;
        // Completion evidence covers every command, not a resource dependency scope.
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        info.commandBufferInfoCount = 1;
        info.pCommandBufferInfos = &command;
        info.waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size());
        info.pWaitSemaphoreInfos = native_waits.data();
        info.signalSemaphoreInfoCount = backing_->timeline ? 1 : 0;
        info.pSignalSemaphoreInfos = backing_->timeline ? &signal : nullptr;
        result = LUX_NATIVE("submit", vkQueueSubmit2(backing_->queue.handle, 1, &info, backing_->fences[slot]));
        if (result != VK_SUCCESS)
            return cxx::unexpected(failure(result));
        ++backing_->submitted;
        return SubmissionTicket{*this, backing_->submitted};
    }

    void SubmissionQueue::cancel() noexcept
    {
        backing_->recording = false;
    }

    RenderResult<std::uint64_t> SubmissionQueue::poll() noexcept
    {
        if (backing_->terminal)
            return cxx::unexpected(*backing_->terminal);
        while (backing_->completed != backing_->submitted)
        {
            const auto slot = static_cast<std::uint32_t>(backing_->completed % capacity());
            const auto result = LUX_NATIVE("fence_status", vkGetFenceStatus(backing_->device, backing_->fences[slot]));
            if (result == VK_NOT_READY)
                break;
            if (result != VK_SUCCESS)
                return cxx::unexpected(failure(result));
            ++backing_->completed;
        }
        return backing_->completed;
    }

    RenderResult<bool> SubmissionQueue::wait(SubmissionTicket ticket, std::uint64_t timeout_ns) noexcept
    {
        if (!owns(ticket))
            return cxx::unexpected(RenderError{kWrongOwner});
        if (backing_->terminal)
            return cxx::unexpected(*backing_->terminal);
        if (ticket.serial() <= backing_->completed)
            return true;
        const auto slot = static_cast<std::uint32_t>((ticket.serial() - 1) % capacity());
        const auto result =
            LUX_NATIVE("wait", vkWaitForFences(backing_->device, 1, &backing_->fences[slot], VK_TRUE, timeout_ns));
        if (result == VK_TIMEOUT)
            return false;
        if (result != VK_SUCCESS)
            return cxx::unexpected(failure(result));
        auto completion = poll();
        if (!completion)
            return cxx::unexpected(completion.error());
        return *completion >= ticket.serial();
    }

    bool SubmissionQueue::owns(SubmissionTicket ticket) const noexcept
    {
        return &ticket.owner() == this && ticket.serial() <= backing_->submitted;
    }

    std::uint64_t SubmissionQueue::completed() const noexcept
    {
        return backing_->completed;
    }

    std::uint64_t SubmissionQueue::submitted() const noexcept
    {
        return backing_->submitted;
    }

    std::uint32_t SubmissionQueue::capacity() const noexcept
    {
        return static_cast<std::uint32_t>(backing_->fences.size());
    }

    VkDevice SubmissionQueue::device() const noexcept
    {
        return backing_->device;
    }

    bool SubmissionQueue::recording() const noexcept
    {
        return backing_->recording;
    }

    bool SubmissionQueue::deviceLost() const noexcept
    {
        return backing_->terminal.has_value();
    }

    NativeQueue SubmissionQueue::nativeQueue() const noexcept
    {
        return backing_->queue;
    }

    CommandBatch::CommandBatch(
        SubmissionQueue &owner, VkCommandBuffer command, std::uint32_t slot, std::uint64_t recording
    ) noexcept
        : owner_(&owner), command_(command), slot_(slot), recording_id_(recording)
    {
    }

    CommandBatch::~CommandBatch() noexcept
    {
        if (owner_)
            owner_->cancel();
    }

    CommandBatch::CommandBatch(CommandBatch &&other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), command_(other.command_), slot_(other.slot_),
          recording_id_(other.recording_id_)
    {
    }

    RenderResult<SubmissionTicket> CommandBatch::submit() && noexcept
    {
        return std::move(*this).submit({});
    }

    RenderResult<SubmissionTicket> CommandBatch::submit(std::span<const SubmissionWait> waits) && noexcept
    {
        auto result = owner_->submit(waits);
        owner_->cancel();
        owner_ = nullptr;
        return result;
    }
} // namespace lux::render::vulkan
