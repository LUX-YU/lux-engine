#include <algorithm>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/execution/ExecutionContext.hpp>
#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/execution/RecordGraph.hpp>
#include <lux/engine/render/upload/ResourceUploader.hpp>

namespace lux::render::detail
{
    FrameExecution::FrameExecution(DeviceSession& device, ResourceUploader* uploads, std::uint32_t count) noexcept
        : device_(device), uploads_(uploads), frame_count_(count)
    {
        frame_receivers_.reserve(uploads ? uploads->windowRegionLimit() : 0);
    }

    RenderResult<std::unique_ptr<FrameExecution>> FrameExecution::create(
        DeviceSession& device,
        ResourceUploader* uploads,
        std::uint32_t count
    ) noexcept
    {
        if (count == 0 || count > 3)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        return std::unique_ptr<FrameExecution>(new FrameExecution(device, uploads, count));
    }

    FrameExecution::~FrameExecution() noexcept
    {
        if (frame_waiters_)
        {
            std::terminate();
        }
    }

    bool FrameExecution::tryBeginWrite(std::uint32_t& slot) noexcept
    {
        slot = next_frame_;
        auto& frame = frames_[slot];
        if (frame.phase == EFramePhase::FREE)
        {
            frame.accepted.serials.fill(0);
            frame.phase = EFramePhase::COLLECTING;
        }
        if (frame.phase != EFramePhase::COLLECTING)
        {
            return false;
        }
        ++frame.pending_writes;
        updateUploadAdmission();
        return true;
    }

    void FrameExecution::finishWrite(std::uint32_t slot, const SubmissionFrontiers& accepted) noexcept
    {
        auto& frame = frames_[slot];
        for (std::size_t i = 0; i < device_.queueCount(); ++i)
        {
            frame.accepted.serials[i] = std::max(frame.accepted.serials[i], accepted.serials[i]);
        }
        --frame.pending_writes;
        updateUploadAdmission();
    }

    void FrameExecution::enqueueWrite(WriteWaiter& node) noexcept
    {
        node.next = nullptr;
        if (frame_waiters_tail_)
        {
            frame_waiters_tail_->next = &node;
        }
        else
        {
            frame_waiters_ = &node;
        }
        frame_waiters_tail_ = &node;
    }

    void FrameExecution::updateUploadAdmission() noexcept
    {
        if (uploads_)
        {
            const auto& frame = frames_[next_frame_];
            uploads_->setBackgroundTransfersAllowed(frame.phase != EFramePhase::PREPARING && frame.pending_writes == 0);
        }
    }

    void FrameExecution::dispatchWrites() noexcept
    {
        const auto phase = frames_[next_frame_].phase;
        const bool can_write = phase == EFramePhase::FREE || phase == EFramePhase::COLLECTING;
        auto* frame_boundary = frame_waiters_tail_;
        while (frame_waiters_ && frame_boundary)
        {
            auto* node = frame_waiters_;
            if (!can_write && !node->stopping(node->operation))
            {
                break;
            }
            frame_waiters_ = node->next;
            if (!frame_waiters_)
            {
                frame_waiters_tail_ = nullptr;
            }
            if (node == frame_boundary)
            {
                frame_boundary = nullptr;
            }
            const auto complete = node->complete;
            auto* operation = node->operation;
            complete(operation);
        }
    }

    void FrameExecution::afterSubmit(void* receiver, void (*complete)(void*, RenderResult<void>) noexcept) noexcept
    {
        frame_receivers_.emplace_back(receiver, complete);
    }

    void FrameExecution::acceptSubmission(std::uint32_t slot, const SubmissionQueue& queue) noexcept
    {
        for (std::size_t i = 0; i < device_.queueCount(); ++i)
        {
            if (&device_.physicalQueue(i) == &queue)
            {
                frames_[slot].accepted.serials[i] = std::max(frames_[slot].accepted.serials[i], queue.submitted());
                return;
            }
        }
    }

    void FrameExecution::finishSubmission(RenderResult<void> result) noexcept
    {
        if (uploads_)
        {
            uploads_->completeFrameCopies(result);
        }
        const auto boundary = frame_receivers_.size();
        for (std::size_t i = 0; i < boundary; ++i)
        {
            // Remove this borrow before delivery. A callback may append work for the next
            // submission and grow the vector; no iterator or record reference crosses it.
            const auto [receiver, complete] = std::exchange(frame_receivers_[i], {});
            complete(receiver, result);
        }
        frame_receivers_.erase(frame_receivers_.begin(), frame_receivers_.begin() + boundary);
    }

    void FrameExecution::sealFrame(FrameSlot& frame) noexcept
    {
        frame.phase = EFramePhase::SUBMITTED;
        frame.pending_queues = 0;
        for (auto serial : frame.accepted.serials)
        {
            frame.pending_queues += serial != 0;
        }
        if (frame.pending_queues == 0)
        {
            frame.phase = EFramePhase::FREE;
            return;
        }
        for (std::size_t i = 0; i < device_.queueCount(); ++i)
        {
            if (frame.accepted.serials[i] == 0)
            {
                continue;
            }
            device_.physicalQueue(i).arm(
                frame.accepted.serials[i],
                frame.completions[i],
                [](void* value, RenderResult<void>) noexcept
                {
                    auto& frame = *static_cast<FrameSlot*>(value);
                    if (--frame.pending_queues == 0)
                    {
                        frame.phase = EFramePhase::FREE;
                    }
                },
                &frame
            );
        }
    }

    void FrameExecution::collectStorageUses(
        std::span<ExecutionContext* const> contexts,
        std::vector<StorageFrameUse>& output
    ) noexcept
    {
        output.clear();
        for (auto* context : contexts)
        {
            for (const auto& use : context->storage_uses_)
            {
                if (use.ops->prepareFrame)
                {
                    output.push_back({use.native, use.ops});
                }
            }
        }
        std::ranges::sort(
            output,
            [](const auto& left, const auto& right) noexcept { return std::less<void*>{}(left.storage, right.storage); }
        );
        output.erase(
            std::unique(
                output.begin(),
                output.end(),
                [](const auto& left, const auto& right) noexcept { return left.storage == right.storage; }
            ),
            output.end()
        );
    }

    RenderResult<std::uint32_t> FrameExecution::render(
        std::span<ExecutionContext* const> contexts,
        std::span<const StorageFrameUse> storages
    ) noexcept
    {
        if (uploads_)
        {
            uploads_->reserveFrameStaging(!contexts.empty());
        }
        cxx::scope_exit update_admission([this]() noexcept { updateUploadAdmission(); });
        auto& frame = frames_[next_frame_];
        if (contexts.empty() && frame.phase == EFramePhase::FREE)
        {
            return 0;
        }
        auto outcome = [&](EExecutionOutcome value) noexcept -> RenderResult<std::uint32_t>
        {
            for (auto* context : contexts)
            {
                ++context->progress_.counts[static_cast<std::size_t>(value)];
            }
            return 0;
        };
        if (frame.phase == EFramePhase::SUBMITTED)
        {
            return outcome(EExecutionOutcome::PREVIOUS_SUBMISSION_PENDING);
        }
        if (frame.phase == EFramePhase::FREE)
        {
            frame.accepted.serials.fill(0);
        }
        frame.phase = EFramePhase::PREPARING;
        if (frame.pending_writes != 0)
        {
            return outcome(EExecutionOutcome::RESOURCE_PREPARATION_PENDING);
        }
        if (contexts.empty())
        {
            sealFrame(frame);
            next_frame_ = (next_frame_ + 1) % frame_count_;
            return 0;
        }
        auto& graphics = device_.queue();
        if (graphics.recording())
        {
            return outcome(EExecutionOutcome::RECORDING_BUSY);
        }
        if (!graphics.hasCapacity())
        {
            return outcome(EExecutionOutcome::QUEUE_FULL);
        }
        // This synchronous phase may fill staging and update descriptors, but may not
        // submit, suspend or dispatch callbacks. Only the following recording segment owns a CommandBuffer.
        if (!storages.empty() && !uploads_)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        bool resources_ready = true;
        for (const auto& use : storages)
        {
            auto prepared = use.ops->prepareFrame(use.storage, next_frame_, *uploads_);
            if (!prepared)
            {
                finishSubmission(cxx::unexpected(prepared.error()));
                sealFrame(frame);
                return cxx::unexpected(prepared.error());
            }
            resources_ready &= *prepared;
        }
        bool contexts_ready = resources_ready;
        bool native_records = false;
        if (resources_ready)
        {
            for (auto* context : contexts)
            {
                auto prepared = context->prepareFrame(next_frame_, uploads_);
                if (!prepared)
                {
                    finishSubmission(cxx::unexpected(prepared.error()));
                    sealFrame(frame);
                    return cxx::unexpected(prepared.error());
                }
                contexts_ready &= *prepared;
                native_records |= !context->prepared_data_.empty();
            }
        }
        const bool has_copies = uploads_ && uploads_->hasFrameCopies();
        if (!contexts_ready && !has_copies && !native_records)
        {
            finishSubmission(cxx::unexpected(RenderError{kDeviceBusy}));
            return outcome(EExecutionOutcome::RESOURCE_PREPARATION_PENDING);
        }
        auto accepted = graphics.recordAndSubmit(
            [&](nri::CommandBuffer& command, std::uint32_t) noexcept -> RenderResult<void>
            {
                if (uploads_)
                {
                    uploads_->recordFrameCopies(command);
                }
                if (resources_ready)
                {
                    for (auto* context : contexts)
                    {
                        recordGraphPreparation(*context, command, next_frame_);
                    }
                }
                if (contexts_ready)
                {
                    for (auto* context : contexts)
                    {
                        recordGraph(*context, command, next_frame_);
                    }
                }
                return {};
            }
        );
        if (!accepted)
        {
            finishSubmission(accepted);
            sealFrame(frame);
            return cxx::unexpected(accepted.error());
        }
        acceptSubmission(next_frame_, graphics);
        finishSubmission({});
        if (!contexts_ready)
        {
            (void)outcome(EExecutionOutcome::RESOURCE_PREPARATION_PENDING);
            return 1; // A real replica-preparation submission; the frame stays open for its next finite window.
        }
        sealFrame(frame);
        for (auto* context : contexts)
        {
            context->submitted(next_frame_);
        }
        next_frame_ = (next_frame_ + 1) % frame_count_;
        return 1;
    }

    RenderResult<bool> FrameExecution::waitForProgress(std::uint64_t timeout_ns) noexcept
    {
        const auto count = device_.queueCount();
        const auto& frame = frames_[next_frame_];
        if (frame.phase == EFramePhase::SUBMITTED)
        {
            for (std::size_t offset = 0; offset < count; ++offset)
            {
                const auto index = (wait_queue_cursor_ + offset) % count;
                auto& queue = device_.physicalQueue(index);
                const auto serial = frame.accepted.serials[index];
                if (serial > queue.completed())
                {
                    wait_queue_cursor_ = static_cast<std::uint8_t>((index + 1) % count);
                    return queue.wait(serial, timeout_ns);
                }
            }
        }
        // No submitted frame is blocking reuse. Rotate through the actual Queue frontiers
        // so background uploads and cleanup do not always wait behind physical queue zero.
        for (std::size_t offset = 0; offset < count; ++offset)
        {
            const auto index = (wait_queue_cursor_ + offset) % count;
            auto& queue = device_.physicalQueue(index);
            if (!queue.idle())
            {
                wait_queue_cursor_ = static_cast<std::uint8_t>((index + 1) % count);
                return queue.wait(queue.completed() + 1, timeout_ns);
            }
        }
        return false;
    }

} // namespace lux::render::detail
