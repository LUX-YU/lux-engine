#include <lux/engine/render/device/SubmissionQueue.hpp>

#include <limits>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <optional>
#include <vector>

namespace lux::render::detail
{
    SubmissionQueue::SubmissionQueue(
        DeviceSession& session,
        nri::Queue& queue,
        QueueProperties properties,
        std::uint32_t capacity
    ) noexcept
        : session_(session), queue_(queue), properties_(properties), batches_(capacity)
    {
    }

    RenderResult<std::unique_ptr<SubmissionQueue>> SubmissionQueue::create(
        DeviceSession& session,
        nri::Queue& queue,
        QueueProperties properties,
        std::uint32_t capacity
    ) noexcept
    {
        if (capacity == 0)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        auto result_queue = std::unique_ptr<SubmissionQueue>(new SubmissionQueue(session, queue, properties, capacity));
        const auto& nri_core = session.nriCore();
        auto result = nri_core.CreateFence(session.device(), 0, result_queue->fence_);
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        for (auto& batch : result_queue->batches_)
        {
            result = nri_core.CreateCommandAllocator(queue, batch.allocator);
            if (result != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
            result = nri_core.CreateCommandBuffer(*batch.allocator, batch.command);
            if (result != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
        }
        return result_queue;
    }

    SubmissionQueue::~SubmissionQueue() noexcept
    {
        if (callbacks_ != nullptr)
        {
            std::terminate();
        }
        completePending();
        const auto& nri_core = session_.nriCore();
        for (auto& batch : batches_)
        {
            if (batch.command)
            {
                nri_core.DestroyCommandBuffer(batch.command);
            }
            if (batch.allocator)
            {
                nri_core.DestroyCommandAllocator(batch.allocator);
            }
        }
        if (fence_)
        {
            nri_core.DestroyFence(fence_);
        }
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
        if (!callbacks_tail_ || callbacks_tail_->serial_ <= serial)
        {
            if (callbacks_tail_)
            {
                callbacks_tail_->next_ = &callback;
            }
            else
            {
                callbacks_ = &callback;
            }
            callback.next_ = nullptr;
            callbacks_tail_ = &callback;
            return;
        }
        auto** link = &callbacks_;
        while (*link && (*link)->serial_ <= serial)
        {
            link = &(*link)->next_;
        }
        callback.next_ = *link;
        *link = &callback;
    }

    void SubmissionQueue::dispatchCompleted() noexcept
    {
        auto* ready = callbacks_;
        auto** end = &callbacks_;
        while (*end && (terminal_ || (*end)->serial_ <= completed_))
        {
            end = &(*end)->next_;
        }
        if (end == &callbacks_)
        {
            return;
        }
        auto* pending = *end;
        *end = nullptr;
        callbacks_ = pending;
        if (!pending)
        {
            callbacks_tail_ = nullptr;
        }
        const auto terminal = terminal_;
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
        if (recording_)
        {
            std::terminate();
        }
        if (!deviceLost() && !idle())
        {
            auto result = waitIdle(UINT64_MAX);
            if (!result || !*result)
            {
                std::terminate();
            }
        }
    }

    RenderError SubmissionQueue::failure(nri::Result result) noexcept
    {
        const auto error = nativeError(result);
        if (result == nri::Result::DEVICE_LOST)
        {
            terminal_ = error;
        }
        return error;
    }

    RenderResult<nri::CommandBuffer*> SubmissionQueue::begin(std::uint32_t& slot) noexcept
    {
        if (terminal_)
        {
            return cxx::unexpected(*terminal_);
        }
        if (recording_)
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        // The device progress phase observes completion and dispatches dependent reuse callbacks.
        // Refreshing the frontier here could reuse a query slot before its completed measurement
        // has been consumed. Recording never advances or re-enters that phase.
        const bool is_full = submitted_ - completed_ == capacity();
        const bool is_exhausted = submitted_ == std::numeric_limits<std::uint64_t>::max();
        if (is_full || is_exhausted)
        {
            return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        slot = static_cast<std::uint32_t>(submitted_ % capacity());
        const auto& batch = batches_[slot];
        auto result = session_.resetCommandAllocator(*batch.allocator);
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        result = session_.nriCore().BeginCommandBuffer(*batch.command, nullptr);
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        recording_ = true;
        return batch.command;
    }

    bool SubmissionQueue::hasCapacity() const noexcept
    {
        return submitted_ - completed_ < capacity();
    }

    RenderResult<void> SubmissionQueue::finishRecording() noexcept
    {
        const auto slot = static_cast<std::uint32_t>(submitted_ % capacity());
        const auto result = session_.nriCore().EndCommandBuffer(*batches_[slot].command);
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        return {};
    }

    RenderResult<void> SubmissionQueue::submit(std::span<const nri::FenceSubmitDesc> waits) noexcept
    {
        const auto slot = static_cast<std::uint32_t>(submitted_ % capacity());
        const nri::FenceSubmitDesc signal{fence_, submitted_ + 1, nri::StageBits::ALL};
        nri::QueueSubmitDesc submission{};
        submission.commandBuffers = &batches_[slot].command;
        submission.commandBufferNum = 1;
        submission.waitFences = waits.data();
        submission.waitFenceNum = static_cast<std::uint32_t>(waits.size());
        submission.signalFences = &signal;
        submission.signalFenceNum = 1;
        const auto result = session_.nriCore().QueueSubmit(queue_, submission);
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        ++submitted_;
        return {};
    }

    void SubmissionQueue::cancel() noexcept
    {
        recording_ = false;
    }

    RenderResult<void> SubmissionQueue::poll() noexcept
    {
        if (terminal_)
        {
            return cxx::unexpected(*terminal_);
        }
        if (idle())
        {
            return {};
        }
        std::uint64_t value = 0;
        const auto result = session_.queryFence(*fence_, value);
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        completed_ = value;
        return {};
    }

    RenderResult<bool> SubmissionQueue::wait(std::uint64_t serial, std::uint64_t timeout_ns) noexcept
    {
        if (terminal_)
        {
            return cxx::unexpected(*terminal_);
        }
        if (serial <= completed_)
        {
            return true;
        }
        bool ready = false;
        const auto result = session_.waitFence(*fence_, serial, timeout_ns, ready);
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        if (ready)
        {
            if (auto observed = poll(); !observed)
            {
                return cxx::unexpected(observed.error());
            }
        }
        return ready;
    }

    RenderResult<bool> SubmissionQueue::waitIdle(std::uint64_t timeout_ns) noexcept
    {
        return wait(submitted_, timeout_ns);
    }

    RenderResult<nri::FenceSubmitDesc> SubmissionQueue::dependency(nri::StageBits stages) const noexcept
    {
        if (terminal_)
        {
            return cxx::unexpected(*terminal_);
        }
        return nri::FenceSubmitDesc{fence_, submitted_, stages};
    }

    SubmissionQueue::CompletionSender SubmissionQueue::whenSubmittedWorkCompletes() noexcept
    {
        return CompletionSender{*this, submitted_};
    }

    SubmissionQueue::CompletionSender SubmissionQueue::whenAvailable() noexcept
    {
        const auto serial = submitted_ < capacity() ? 0 : submitted_ - capacity() + 1;
        return CompletionSender{*this, serial};
    }

    std::uint64_t SubmissionQueue::completed() const noexcept
    {
        return completed_;
    }

    std::uint64_t SubmissionQueue::submitted() const noexcept
    {
        return submitted_;
    }

    std::uint32_t SubmissionQueue::capacity() const noexcept
    {
        return static_cast<std::uint32_t>(batches_.size());
    }

    bool SubmissionQueue::recording() const noexcept
    {
        return recording_;
    }

    bool SubmissionQueue::idle() const noexcept
    {
        return completed_ == submitted_;
    }

    bool SubmissionQueue::deviceLost() const noexcept
    {
        return terminal_.has_value();
    }

    nri::Queue& SubmissionQueue::native() const noexcept
    {
        return queue_;
    }

    const QueueProperties& SubmissionQueue::properties() const noexcept
    {
        return properties_;
    }
} // namespace lux::render::detail
