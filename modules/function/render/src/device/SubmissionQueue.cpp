#include <lux/engine/render/device/SubmissionQueue.hpp>

#include <algorithm>
#include <limits>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <optional>
#include <vector>

namespace lux::render::detail
{
    SubmissionQueue::SubmissionQueue(DeviceSession& session, nri::Queue& queue, QueueProperties properties) noexcept
        : session_(session), queue_(queue), properties_(properties)
    {
    }

    RenderResult<std::unique_ptr<SubmissionQueue>>
    SubmissionQueue::create(DeviceSession& session, nri::Queue& queue, QueueProperties properties) noexcept
    {
        auto result_queue = std::unique_ptr<SubmissionQueue>(new SubmissionQueue(session, queue, properties));
        const auto& nri_core = session.nriCore();
        auto result = nri_core.CreateFence(session.device(), nri::FenceDesc{}, result_queue->fence_);
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        return result_queue;
    }

    SubmissionQueue::~SubmissionQueue() noexcept
    {
        if (callbacks_ != nullptr)
        {
            std::terminate();
        }
        // Destruction is not a hidden wait. The owning scope must settle CPU/native and GPU use first.
        if (submitting_ || (!idle() && !deviceLost()))
        {
            std::terminate();
        }
        const auto& nri_core = session_.nriCore();
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
        if (callback.armed_)
        {
            std::terminate();
        }
        if (serial > submitted_)
        {
            function(user, cxx::unexpected(RenderError{kDeviceArgument}));
            return;
        }
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

    RenderError SubmissionQueue::failure(nri::Result result) noexcept
    {
        const auto error = nativeError(result);
        if (result == nri::Result::DEVICE_LOST)
        {
            terminal_ = error;
        }
        return error;
    }

    exec::task<RenderResult<std::uint64_t>> SubmissionQueue::submit(
        std::span<nri::CommandBuffer* const> commands,
        std::span<const nri::FenceSubmitDesc> waits,
        std::span<const nri::FenceSubmitDesc> signals
    ) noexcept
    {
        const auto owner = co_await stdexec::read_env(stdexec::get_scheduler);
        if (terminal_)
        {
            co_return cxx::unexpected(*terminal_);
        }
        if (submitting_)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
        }
        const bool is_invalid_count =
            commands.size() > UINT32_MAX || waits.size() > UINT32_MAX || signals.size() >= UINT32_MAX;
        if (is_invalid_count)
        {
            co_return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (submitted_ == UINT64_MAX)
        {
            co_return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        submitting_ = true;
        cxx::scope_exit release([this]() noexcept { submitting_ = false; });
        const auto serial = submitted_ + 1;
        auto& storage = submission_storage_;
        storage.commands.assign(commands.begin(), commands.end());
        storage.waits.assign(waits.begin(), waits.end());
        storage.signals.assign(signals.begin(), signals.end());
        storage.signals.push_back({fence_, serial, nri::StageBits::ALL});
        const nri::QueueSubmitDesc submission{
            storage.waits.data(),
            static_cast<std::uint32_t>(storage.waits.size()),
            storage.commands.data(),
            static_cast<std::uint32_t>(storage.commands.size()),
            storage.signals.data(),
            static_cast<std::uint32_t>(storage.signals.size())
        };
        // The worker borrows only native facts and arrays frozen until the owner adopts the result.
        auto result = co_await (
            stdexec::schedule(session_.nativeScheduler()) |
            stdexec::then(
                [core = &session_.nriCore(), queue = &queue_, submission]() noexcept -> RenderResult<void>
                {
                    const auto result = core->QueueSubmit(*queue, submission);
                    if (result == nri::Result::SUCCESS)
                    {
                        return {};
                    }
                    const bool rejected = result == nri::Result::INVALID_ARGUMENT ||
                                          result == nri::Result::UNSUPPORTED || result == nri::Result::OUT_OF_MEMORY;
                    if (rejected || result == nri::Result::DEVICE_LOST)
                    {
                        return cxx::unexpected(nativeError(result));
                    }
                    // Exceptional uncertain acceptance: retire native payloads before returning UNKNOWN.
                    // This is never the ordinary submit/reuse path, and never fabricates a completed serial.
                    const auto retired = core->QueueWaitIdle(queue);
                    if (retired != nri::Result::SUCCESS && retired != nri::Result::DEVICE_LOST)
                    {
                        std::terminate();
                    }
                    return cxx::unexpected(RenderError{kOutcomeUnknown, {static_cast<std::uint64_t>(result)}});
                }
            ) |
            stdexec::upon_error(NativeScheduleFailure<void>{}) |
            stdexec::upon_stopped(
                []() noexcept -> RenderResult<void> { return cxx::unexpected(RenderError{kNativeExecutionClosed}); }
            ) |
            stdexec::continues_on(owner) | stdexec::upon_error(NativeDeliveryFailure<void>{})
        );
        if (!result)
        {
            const auto error = result.error();
            const bool lost = isNativeFailure(error, nri::Result::DEVICE_LOST);
            if (lost || error.type == kOutcomeUnknown)
            {
                terminal_ = error;
                terminal_retired_ = true;
            }
            co_return cxx::unexpected(error);
        }
        submitted_ = serial;
        co_return serial;
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
        completed_ = std::min(value, submitted_); // A native signal may precede Server adoption.
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
        if (submitting_)
        {
            return false;
        }
        return wait(submitted_, timeout_ns);
    }

    RenderResult<nri::FenceSubmitDesc> SubmissionQueue::dependency(std::uint64_t serial, nri::StageBits stages)
        const noexcept
    {
        if (terminal_)
        {
            return cxx::unexpected(*terminal_);
        }
        if (serial > submitted_)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        return nri::FenceSubmitDesc{fence_, serial, stages};
    }

    SubmissionQueue::CompletionSender SubmissionQueue::whenSubmittedWorkCompletes() noexcept
    {
        return CompletionSender{*this, submitted_};
    }

    SubmissionQueue::CompletionSender SubmissionQueue::whenSubmissionCompletes(std::uint64_t serial) noexcept
    {
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

    bool SubmissionQueue::idle() const noexcept
    {
        return !submitting_ && (terminal_retired_ || completed_ == submitted_);
    }

    bool SubmissionQueue::deviceLost() const noexcept
    {
        return terminal_ && isNativeFailure(*terminal_, nri::Result::DEVICE_LOST);
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
