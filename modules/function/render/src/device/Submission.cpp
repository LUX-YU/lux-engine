#include <lux/engine/render/device/Submission.hpp>

#include <limits>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <optional>
#include <vector>

namespace lux::render::detail
{
    struct SubmittedBatch final
    {
        nri::CommandAllocator* allocator = nullptr;
        nri::CommandBuffer* command = nullptr;
    };

    struct SubmissionQueue::Backing final
    {
        DeviceSession& session;
        nri::Queue& queue;
        QueueProperties properties;
        nri::Fence* fence = nullptr;
        std::vector<SubmittedBatch> batches;
        std::uint64_t submitted = 0;
        std::uint64_t completed = 0;
        bool recording = false;
        std::optional<RenderError> terminal;
        SubmissionCallback* callbacks = nullptr;
        SubmissionCallback* callbacks_tail = nullptr;

        Backing(DeviceSession& session, nri::Queue& queue, QueueProperties properties, std::uint32_t capacity)
            : session(session), queue(queue), properties(properties), batches(capacity)
        {
        }

        ~Backing() noexcept
        {
            const auto& api = session.api();
            for (auto& batch : batches)
            {
                if (batch.command)
                {
                    api.DestroyCommandBuffer(batch.command);
                }
                if (batch.allocator)
                {
                    api.DestroyCommandAllocator(batch.allocator);
                }
            }
            if (fence)
            {
                api.DestroyFence(fence);
            }
        }
    };

    SubmissionQueue::SubmissionQueue(std::unique_ptr<Backing> backing) noexcept : backing_(std::move(backing)) {}

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
        auto backing = std::make_unique<Backing>(session, queue, properties, capacity);
        const auto& api = session.api();
        auto result = api.CreateFence(session.device(), 0, backing->fence);
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        for (auto& batch : backing->batches)
        {
            result = api.CreateCommandAllocator(queue, batch.allocator);
            if (result != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
            result = api.CreateCommandBuffer(*batch.allocator, batch.command);
            if (result != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
        }
        return std::unique_ptr<SubmissionQueue>{new SubmissionQueue(std::move(backing))};
    }

    SubmissionQueue::~SubmissionQueue() noexcept
    {
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
        if (backing_->recording)
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
            backing_->terminal = error;
        }
        return error;
    }

    RenderResult<nri::CommandBuffer*> SubmissionQueue::begin(std::uint32_t& slot) noexcept
    {
        if (backing_->terminal)
        {
            return cxx::unexpected(*backing_->terminal);
        }
        if (backing_->recording)
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        // The device progress phase observes completion and dispatches dependent reuse callbacks.
        // Refreshing the frontier here could reuse a query slot before its completed measurement
        // has been consumed. Recording never advances or re-enters that phase.
        const bool is_full = backing_->submitted - backing_->completed == capacity();
        const bool is_exhausted = backing_->submitted == std::numeric_limits<std::uint64_t>::max();
        if (is_full || is_exhausted)
        {
            return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        slot = static_cast<std::uint32_t>(backing_->submitted % capacity());
        const auto& batch = backing_->batches[slot];
        auto result = backing_->session.vk().ResetCommandAllocatorCheckedVK(*batch.allocator);
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        result = backing_->session.api().BeginCommandBuffer(*batch.command, nullptr);
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        backing_->recording = true;
        return batch.command;
    }

    bool SubmissionQueue::hasCapacity() const noexcept
    {
        return backing_->submitted - backing_->completed < capacity();
    }

    RenderResult<void> SubmissionQueue::finishRecording() noexcept
    {
        const auto slot = static_cast<std::uint32_t>(backing_->submitted % capacity());
        const auto result = backing_->session.api().EndCommandBuffer(*backing_->batches[slot].command);
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        return {};
    }

    RenderResult<void> SubmissionQueue::submit(std::span<const nri::FenceSubmitDesc> waits) noexcept
    {
        const auto slot = static_cast<std::uint32_t>(backing_->submitted % capacity());
        const nri::FenceSubmitDesc signal{backing_->fence, backing_->submitted + 1, nri::StageBits::ALL};
        nri::QueueSubmitDesc submission{};
        submission.commandBuffers = &backing_->batches[slot].command;
        submission.commandBufferNum = 1;
        submission.waitFences = waits.data();
        submission.waitFenceNum = static_cast<std::uint32_t>(waits.size());
        submission.signalFences = &signal;
        submission.signalFenceNum = 1;
        const auto result = backing_->session.api().QueueSubmit(backing_->queue, submission);
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        ++backing_->submitted;
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
        if (idle())
        {
            return {};
        }
        std::uint64_t value = 0;
        const auto result = backing_->session.vk().GetFenceValueCheckedVK(*backing_->fence, value);
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(failure(result));
        }
        backing_->completed = value;
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
        bool ready = false;
        const auto result = backing_->session.vk().WaitFenceCheckedVK(*backing_->fence, serial, timeout_ns, ready);
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
        return wait(backing_->submitted, timeout_ns);
    }

    RenderResult<nri::FenceSubmitDesc> SubmissionQueue::dependency(nri::StageBits stages) const noexcept
    {
        if (backing_->terminal)
        {
            return cxx::unexpected(*backing_->terminal);
        }
        return nri::FenceSubmitDesc{backing_->fence, backing_->submitted, stages};
    }

    SubmissionQueue::CompletionSender SubmissionQueue::whenIdle() noexcept
    {
        return CompletionSender{*this, backing_->submitted};
    }

    SubmissionQueue::CompletionSender SubmissionQueue::whenAvailable() noexcept
    {
        const auto serial = backing_->submitted < capacity() ? 0 : backing_->submitted - capacity() + 1;
        return CompletionSender{*this, serial};
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

    nri::Queue& SubmissionQueue::native() const noexcept
    {
        return backing_->queue;
    }

    const QueueProperties& SubmissionQueue::properties() const noexcept
    {
        return backing_->properties;
    }
} // namespace lux::render::detail
