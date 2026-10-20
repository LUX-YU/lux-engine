#include <lux/engine/render/device/Submission.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <lux/engine/gapi/vk/Semaphore.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <optional>
#include <utility>
#include <vector>

namespace lux::render::detail
{
    // Physical submission slots own command buffers and fences, not business resources.
    struct SubmittedBatch final
    {
        VkCommandBuffer command{};
        VkFence fence{};
    };

    struct SubmissionQueue::Backing
    {
        VkDevice device;
        NativeQueue queue;
        gapi::vk::Semaphore timeline;
        VkCommandPool pool{};
        std::vector<SubmittedBatch> batches;
        std::uint64_t submitted{};
        std::uint64_t completed{};
        std::uint64_t recording_id{};
        bool recording{};
        std::optional<RenderError> terminal;
        SubmissionCallback* callbacks = nullptr;
        SubmissionCallback* callbacks_tail = nullptr;

        Backing(VkDevice device, NativeQueue queue, std::uint32_t capacity)
            : device(device), queue(queue), batches(capacity)
        {
        }

        ~Backing() noexcept
        {
            if (timeline)
            {
                timeline.reset();
            }
            for (auto& batch : batches)
            {
                if (batch.fence)
                {
                    vkDestroyFence(device, batch.fence, nullptr);
                }
            }
            if (pool)
            {
                vkDestroyCommandPool(device, pool, nullptr);
            }
        }
    };

    SubmissionQueue::SubmissionQueue(std::unique_ptr<Backing> backing) noexcept : backing_(std::move(backing)) {}

    RenderResult<std::unique_ptr<SubmissionQueue>> SubmissionQueue::create(
        VkDevice device,
        NativeQueue native_queue,
        bool timeline_enabled,
        std::uint32_t capacity
    ) noexcept
    {
        if (capacity == 0)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        auto backing = std::make_unique<Backing>(device, native_queue, capacity);
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = backing->queue.family;
        auto result = vkCreateCommandPool(device, &pool, nullptr, &backing->pool);
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = backing->pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = capacity;
        std::vector<VkCommandBuffer> commands(capacity);
        result = vkAllocateCommandBuffers(device, &allocation, commands.data());
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        const VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (std::size_t i = 0; i < backing->batches.size(); ++i)
        {
            auto& batch = backing->batches[i];
            batch.command = commands[i];
            result = vkCreateFence(device, &fence, nullptr, &batch.fence);
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
        }
        if (timeline_enabled)
        {
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            info.pNext = &type;
            VkSemaphore timeline{};
            result = vkCreateSemaphore(device, &info, nullptr, &timeline);
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
            backing->timeline = gapi::vk::Semaphore::adopt(device, timeline);
        }
        return std::unique_ptr<SubmissionQueue>{new SubmissionQueue(std::move(backing))};
    }

    SubmissionQueue::~SubmissionQueue() noexcept
    {
        // Completion nodes borrow this queue and must finish before teardown.
        if (backing_->callbacks != nullptr)
        {
            std::terminate();
        }
        completePending();
    }

    SubmissionCallback::~SubmissionCallback() noexcept
    {
        reset();
    }

    void SubmissionCallback::reset() noexcept
    {
        if (armed_)
        {
            std::terminate();
        }
    }

    void SubmissionQueue::arm(
        std::uint64_t serial,
        SubmissionCallback& callback,
        SubmissionCallbackFn function,
        void* user
    ) noexcept
    {
        callback.callback_ = function;
        callback.user_ = user;
        callback.serial_ = serial;
        callback.armed_ = true;
        if (!backing_->callbacks_tail || backing_->callbacks_tail->serial_ <= serial)
        {
            if (backing_->callbacks_tail)
            {
                backing_->callbacks_tail->next_ = &callback;
            }
            else
            {
                backing_->callbacks = &callback;
            }
            callback.next_ = nullptr;
            backing_->callbacks_tail = &callback;
            return;
        }
        auto** link = &backing_->callbacks;
        while (*link && (*link)->serial_ <= serial)
        {
            link = &(*link)->next_;
        }
        callback.next_ = *link;
        *link = &callback;
    }

    void SubmissionQueue::dispatchCompleted() noexcept
    {
        auto* ready = backing_->callbacks;
        auto** end = &backing_->callbacks;
        while (*end && (backing_->terminal || (*end)->serial_ <= backing_->completed))
        {
            end = &(*end)->next_;
        }
        if (end == &backing_->callbacks)
        {
            return;
        }
        auto* pending = *end;
        *end = nullptr;
        backing_->callbacks = pending;
        if (!pending)
        {
            backing_->callbacks_tail = nullptr;
        }
        const auto terminal = backing_->terminal;
        // Detached ready-prefix snapshot: callback re-registration cannot extend this dispatch indefinitely.
        while (ready)
        {
            auto* current = ready;
            ready = current->next_;
            const auto function = current->callback_;
            auto* user = current->user_;
            current->next_ = nullptr;
            current->armed_ = false;
            function(user, terminal ? RenderResult<void>{cxx::unexpected(*terminal)} : RenderResult<void>{});
            // The callback may destroy its node and enclosing operation. Never access current again.
        }
    }

    void SubmissionQueue::completePending() noexcept
    {
        // A live CommandBatch is a CPU borrow and must not outlive its owner.
        // Final teardown joins admitted GPU work by fence, without a public
        // shutdown protocol or vkDeviceWaitIdle on individual resources.
        if (backing_->recording)
        {
            std::terminate();
        }
        const bool has_pending = backing_->submitted != backing_->completed;
        if (!deviceLost() && has_pending)
        {
            auto result = waitIdle(UINT64_MAX);
            const bool cannot_teardown = !deviceLost() && (!result || !*result);
            if (cannot_teardown)
            {
                std::terminate();
            }
        }
    }

    RenderError SubmissionQueue::failure(VkResult result) noexcept
    {
        auto error = nativeError(result);
        if (result == VK_ERROR_DEVICE_LOST)
        {
            backing_->terminal = error;
        }
        return error;
    }

    RenderResult<CommandBatch> SubmissionQueue::begin() noexcept
    {
        if (backing_->terminal)
        {
            return cxx::unexpected(*backing_->terminal);
        }
        if (backing_->recording)
        {
            return cxx::unexpected(RenderError{kBusy});
        }
        auto completion = poll();
        if (!completion)
        {
            return cxx::unexpected(completion.error());
        }
        return beginAfterPoll();
    }

    RenderResult<CommandBatch> SubmissionQueue::beginAfterPoll() noexcept
    {
        const bool is_full = backing_->submitted - backing_->completed == capacity();
        const bool is_exhausted = backing_->submitted == std::numeric_limits<std::uint64_t>::max() ||
                                  backing_->recording_id == std::numeric_limits<std::uint64_t>::max();
        if (is_full || is_exhausted)
        {
            return cxx::unexpected(RenderError{kCapacity});
        }
        const auto slot = static_cast<std::uint32_t>(backing_->submitted % capacity());
        const auto command = backing_->batches[slot].command;
        auto result = vkResetCommandBuffer(command, 0);
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command, &begin);
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        backing_->recording = true;
        ++backing_->recording_id;
        return CommandBatch{*this, command, slot, backing_->recording_id};
    }

    RenderResult<void> SubmissionQueue::submit(
        std::span<const VkSemaphoreSubmitInfo> waits,
        VkSemaphore acquired,
        VkSemaphore present,
        SubmissionCallback* completion,
        SubmissionCallbackFn function,
        void* user
    ) noexcept
    {
        // Admission owns all mutation; no callback can change the prepared recording here.
        const bool invalid_completion = completion && (completion->armed_ || !function);
        if (invalid_completion || waits.size() > 3)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        std::array<VkSemaphoreSubmitInfo, 4> native_waits{};
        std::ranges::copy(waits, native_waits.begin());
        const bool incomplete_wsi = (acquired == VK_NULL_HANDLE) != (present == VK_NULL_HANDLE);
        if (incomplete_wsi || (acquired && acquired == present))
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        if (acquired)
        {
            auto& wait = native_waits[waits.size()];
            wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            wait.semaphore = acquired;
            wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
        const auto slot = static_cast<std::uint32_t>(backing_->submitted % capacity());
        auto result = vkEndCommandBuffer(backing_->batches[slot].command);
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        result = vkResetFences(backing_->device, 1, &backing_->batches[slot].fence);
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        command.commandBuffer = backing_->batches[slot].command;
        std::array<VkSemaphoreSubmitInfo, 2> signals{};
        auto& signal = signals[0];
        signal = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = backing_->timeline;
        signal.value = backing_->submitted + 1;
        // Completion evidence covers every command, not a resource dependency scope.
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        std::uint32_t signal_count = backing_->timeline ? 1 : 0;
        if (present)
        {
            auto& output = signals[signal_count++];
            output = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            output.semaphore = present;
            output.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
        VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        info.commandBufferInfoCount = 1;
        info.pCommandBufferInfos = &command;
        info.waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size()) + (acquired != VK_NULL_HANDLE);
        info.pWaitSemaphoreInfos = native_waits.data();
        info.signalSemaphoreInfoCount = signal_count;
        info.pSignalSemaphoreInfos = signals.data();
        result = vkQueueSubmit2(backing_->queue.handle, 1, &info, backing_->batches[slot].fence);
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        ++backing_->submitted;
        backing_->recording = false;
        if (completion)
        {
            arm(backing_->submitted, *completion, function, user);
        }
        return {};
    }

    void SubmissionQueue::cancel() noexcept
    {
        backing_->recording = false;
    }

    RenderResult<void> SubmissionQueue::poll() noexcept
    {
        if (backing_->terminal)
        {
            return cxx::unexpected(*backing_->terminal);
        }
        while (backing_->completed != backing_->submitted)
        {
            const auto slot = static_cast<std::uint32_t>(backing_->completed % capacity());
            const auto result = vkGetFenceStatus(backing_->device, backing_->batches[slot].fence);
            if (result == VK_NOT_READY)
            {
                break;
            }
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(failure(result));
            }
            ++backing_->completed;
        }
        return {};
    }

    RenderResult<bool> SubmissionQueue::wait(std::uint64_t serial, std::uint64_t timeout_ns) noexcept
    {
        if (backing_->terminal)
        {
            return cxx::unexpected(*backing_->terminal);
        }
        if (serial <= backing_->completed)
        {
            return true;
        }
        const auto slot = static_cast<std::uint32_t>((serial - 1) % capacity());
        const auto result = vkWaitForFences(backing_->device, 1, &backing_->batches[slot].fence, VK_TRUE, timeout_ns);
        if (result == VK_TIMEOUT)
        {
            return false;
        }
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        auto completion = poll();
        if (!completion)
        {
            return cxx::unexpected(completion.error());
        }
        return backing_->completed >= serial;
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
        return static_cast<std::uint32_t>(backing_->batches.size());
    }

    VkDevice SubmissionQueue::device() const noexcept
    {
        return backing_->device;
    }

    bool SubmissionQueue::recording() const noexcept
    {
        return backing_->recording;
    }

    bool SubmissionQueue::idle() const noexcept
    {
        return backing_->completed == backing_->submitted;
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
        SubmissionQueue& owner,
        VkCommandBuffer command,
        std::uint32_t slot,
        std::uint64_t recording
    ) noexcept
        : owner_(&owner), command_(command), slot_(slot), recording_id_(recording)
    {
    }

    CommandBatch::~CommandBatch() noexcept
    {
        if (owner_)
        {
            owner_->cancel();
        }
    }

    CommandBatch::CommandBatch(CommandBatch&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), command_(other.command_), slot_(other.slot_),
          recording_id_(other.recording_id_)
    {
    }

    RenderResult<void> CommandBatch::submit() && noexcept
    {
        return std::move(*this).submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    }

    RenderResult<void> CommandBatch::submit(
        SubmissionCallback& completion,
        SubmissionCallbackFn function,
        void* user
    ) && noexcept
    {
        auto result = owner_->submit({}, {}, {}, &completion, function, user);
        if (!result)
        {
            owner_->cancel();
        }
        owner_ = nullptr;
        return result;
    }

    RenderResult<void> CommandBatch::submitAfter(
        const SubmissionQueue& source,
        VkPipelineStageFlags2 stages
    ) && noexcept
    {
        if (&source == owner_)
        {
            return std::move(*this).submit();
        }
        if (source.device() != owner_->device())
        {
            return cxx::unexpected(RenderError{kDeviceMismatch});
        }
        VkPipelineStageFlags2 allowed = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
                                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                                        VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
        const auto flags = owner_->nativeQueue().flags;
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
        const bool invalid_stages = !stages || (stages & ~allowed) != 0;
        if (invalid_stages)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        auto wait = source.dependency(stages);
        if (!wait)
        {
            return cxx::unexpected(wait.error());
        }
        auto result = owner_->submit(std::span(&*wait, 1));
        if (!result)
        {
            owner_->cancel();
        }
        owner_ = nullptr;
        return result;
    }

    RenderResult<void> CommandBatch::submit(VkSemaphore acquired, VkSemaphore present) && noexcept
    {
        auto result = owner_->submit({}, acquired, present);
        if (!result)
        {
            owner_->cancel();
        }
        owner_ = nullptr;
        return result;
    }

    RenderResult<VkSemaphoreSubmitInfo> SubmissionQueue::dependency(VkPipelineStageFlags2 stages) const noexcept
    {
        if (!backing_->timeline)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        VkSemaphoreSubmitInfo result{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        result.semaphore = backing_->timeline;
        result.value = backing_->submitted;
        result.stageMask = stages;
        return result;
    }

    RenderResult<bool> SubmissionQueue::waitIdle(std::uint64_t timeout_ns) noexcept
    {
        return wait(backing_->submitted, timeout_ns);
    }

    SubmissionQueue::CompletionSender SubmissionQueue::whenIdle() noexcept
    {
        return CompletionSender{*this, backing_->submitted};
    }

    SubmissionQueue::CompletionSender SubmissionQueue::whenAvailable() noexcept
    {
        const bool full = backing_->submitted - backing_->completed == capacity();
        return CompletionSender{*this, backing_->completed + std::uint64_t(full)};
    }
} // namespace lux::render::detail
