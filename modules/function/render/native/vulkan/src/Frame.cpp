#include <algorithm>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>

namespace lux::render::vulkan
{
    bool UploadContext::tryBeginFrameWrite(std::uint32_t& slot) noexcept
    {
        slot = session.next_frame_;
        auto& frame = session.frames_[slot];
        if (frame.phase == DeviceSession::EFramePhase::FREE)
        {
            frame.accepted_serials.fill(0);
            frame.phase = DeviceSession::EFramePhase::COLLECTING;
        }
        if (frame.phase != DeviceSession::EFramePhase::COLLECTING)
        {
            return false;
        }
        ++frame.pending_writes;
        return true;
    }

    void UploadContext::finishFrameWrite(std::uint32_t slot) noexcept
    {
        auto& frame = session.frames_[slot];
        --frame.pending_writes;
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

    RenderResult<std::uint32_t> DeviceSession::render(std::span<ExecutionContext* const> contexts) noexcept
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
        auto batch = graphics.beginAfterPoll();
        if (!batch)
        {
            return batch.error().type == kCapacity ? outcome(EExecutionOutcome::QUEUE_FULL)
                                                   : RenderResult<std::uint32_t>{cxx::unexpected(batch.error())};
        }
        const auto failed = [&](RenderError error) noexcept -> RenderResult<std::uint32_t>
        {
            if (uploads_)
            {
                uploads_->finishFrameCopies(cxx::unexpected(error));
            }
            sealFrame(frame);
            return cxx::unexpected(error);
        };
        // Scratch contains only the actual cold-qualified borrows used in this frame. It is
        // rebuilt from active Contexts, never an owning or global professional resource table.
        frame_storages_.clear();
        for (auto* context : contexts)
        {
            for (const auto& use : context->storage_uses_)
            {
                if (use.ops->prepareFrame)
                {
                    frame_storages_.emplace_back(use.native, use.ops);
                }
            }
        }
        std::ranges::sort(
            frame_storages_,
            [](const auto& left, const auto& right) { return std::less<void*>{}(left.first, right.first); }
        );
        frame_storages_.erase(
            std::unique(
                frame_storages_.begin(),
                frame_storages_.end(),
                [](const auto& left, const auto& right) { return left.first == right.first; }
            ),
            frame_storages_.end()
        );
        if (!frame_storages_.empty() && !uploads_)
        {
            return failed(RenderError{kInvalidArgument});
        }
        bool resources_ready = true;
        for (const auto& [storage, ops] : frame_storages_)
        {
            auto prepared = ops->prepareFrame(storage, next_frame_, *uploads_);
            if (!prepared)
            {
                return failed(prepared.error());
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
                    return failed(prepared.error());
                }
                contexts_ready &= *prepared;
                native_records |= !context->prepared_data_.empty();
            }
        }
        const bool has_copies = uploads_ && uploads_->hasFrameCopies();
        if (!contexts_ready && !has_copies && !native_records)
        {
            if (uploads_)
            {
                uploads_->finishFrameCopies(cxx::unexpected(RenderError{kCapacity}));
            }
            return outcome(EExecutionOutcome::RESOURCE_PREPARATION_PENDING);
        }
        if (uploads_)
        {
            uploads_->recordFrame(*batch);
        }
        if (resources_ready)
        {
            for (auto* context : contexts)
            {
                context->recordPreparation(*batch, next_frame_);
            }
        }
        if (contexts_ready)
        {
            for (auto* context : contexts)
            {
                context->record(*batch, next_frame_);
            }
        }
        auto accepted = std::move(*batch).submit();
        if (!accepted)
        {
            return failed(accepted.error());
        }
        frame.accepted_serials[queue_roles_[static_cast<std::size_t>(EQueueRole::GRAPHICS)]] = graphics.submitted();
        if (uploads_)
        {
            uploads_->finishFrameCopies({});
        }
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
} // namespace lux::render::vulkan
