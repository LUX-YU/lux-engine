#include <algorithm>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/execution/ExecutionContext.hpp>
#include <lux/engine/render/execution/RecordGraph.hpp>
#include <lux/engine/render/upload/UploadContext.hpp>

namespace lux::render::detail
{
    bool DeviceSession::tryBeginFrameWrite(std::uint32_t& slot) noexcept
    {
        slot = next_frame_;
        auto& frame = frames_[slot];
        if (frame.phase == EFramePhase::FREE)
        {
            frame.accepted_serials.fill(0);
            frame.phase = EFramePhase::COLLECTING;
        }
        if (frame.phase != EFramePhase::COLLECTING)
        {
            return false;
        }
        ++frame.pending_writes;
        return true;
    }

    void DeviceSession::finishFrameWrite(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        --frame.pending_writes;
    }

    void DeviceSession::enqueueFrameWrite(FrameWriteWaiter& node) noexcept
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

    bool DeviceSession::backgroundUploadsAllowed() const noexcept
    {
        const auto& frame = frames_[next_frame_];
        return frame.phase != EFramePhase::PREPARING && frame.pending_writes == 0;
    }

    void DeviceSession::dispatchFrameWrites() noexcept
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

    void DeviceSession::afterFrameSubmit(void* receiver, void (*complete)(void*, RenderResult<void>) noexcept) noexcept
    {
        frame_receivers_.emplace_back(receiver, complete);
    }

    void DeviceSession::acceptFrameSubmission(std::uint32_t slot, const SubmissionQueue& queue) noexcept
    {
        for (std::size_t i = 0; i < physical_queues_.size(); ++i)
        {
            if (physical_queues_[i].get() == &queue)
            {
                frames_[slot].accepted_serials[i] = queue.submitted();
                return;
            }
        }
    }

    void DeviceSession::finishFrameSubmission(RenderResult<void> result) noexcept
    {
        if (uploads_)
        {
            uploads_->finishFrameCopies(result);
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

    void DeviceSession::sealFrame(FrameSlot& frame) noexcept
    {
        frame.phase = EFramePhase::SUBMITTED;
        frame.pending_queues = 0;
        for (auto serial : frame.accepted_serials)
        {
            frame.pending_queues += serial != 0;
        }
        if (frame.pending_queues == 0)
        {
            frame.phase = EFramePhase::FREE;
            return;
        }
        for (std::size_t i = 0; i < physical_queues_.size(); ++i)
        {
            if (frame.accepted_serials[i] == 0)
            {
                continue;
            }
            physical_queues_[i]->arm(
                frame.accepted_serials[i],
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

    void DeviceSession::collectFrameStorages(
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

    RenderResult<std::uint32_t> DeviceSession::render(
        std::span<ExecutionContext* const> contexts,
        std::span<const StorageFrameUse> storages
    ) noexcept
    {
        rendering_ = !contexts.empty();
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
            frame.accepted_serials.fill(0);
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
        auto& graphics = queue();
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
                finishFrameSubmission(cxx::unexpected(prepared.error()));
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
                auto prepared = context->prepareFrame(next_frame_, uploads_.get());
                if (!prepared)
                {
                    finishFrameSubmission(cxx::unexpected(prepared.error()));
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
            finishFrameSubmission(cxx::unexpected(RenderError{kDeviceBusy}));
            return outcome(EExecutionOutcome::RESOURCE_PREPARATION_PENDING);
        }
        auto accepted = graphics.recordAndSubmit(
            [&](nri::CommandBuffer& command, std::uint32_t) noexcept -> RenderResult<void>
            {
                if (uploads_)
                {
                    uploads_->recordFrame(command);
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
            finishFrameSubmission(accepted);
            sealFrame(frame);
            return cxx::unexpected(accepted.error());
        }
        acceptFrameSubmission(next_frame_, graphics);
        finishFrameSubmission({});
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
} // namespace lux::render::detail
