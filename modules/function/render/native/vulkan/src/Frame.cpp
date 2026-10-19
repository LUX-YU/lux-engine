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
        frame.accepted_serials[session.queue_roles_[0]] = session.queue().submitted();
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
        for (auto* context : contexts)
        {
            if (auto prepared = context->prepareFrame(); !prepared)
            {
                sealFrame(frame);
                return cxx::unexpected(prepared.error());
            }
        }
        auto batch = graphics.beginAfterPoll();
        if (!batch)
        {
            return batch.error().type == kCapacity ? outcome(EExecutionOutcome::QUEUE_FULL)
                                                   : RenderResult<std::uint32_t>{cxx::unexpected(batch.error())};
        }
        for (auto* context : contexts)
        {
            context->record(*batch);
        }
        if (auto accepted = std::move(*batch).submit(); !accepted)
        {
            sealFrame(frame);
            return cxx::unexpected(accepted.error());
        }
        frame.accepted_serials[queue_roles_[static_cast<std::size_t>(EQueueRole::GRAPHICS)]] = graphics.submitted();
        sealFrame(frame);
        for (auto* context : contexts)
        {
            context->submitted();
        }
        next_frame_ = (next_frame_ + 1) % frame_count_;
        return 1;
    }
} // namespace lux::render::vulkan
