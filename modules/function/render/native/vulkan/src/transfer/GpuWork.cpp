#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>

namespace lux::render::vulkan
{
    GpuWork GpuWork::accepted(DeviceSession& session, SubmissionTicket ticket, void* owner,
        void (*destroy)(void*) noexcept, RenderResult<void> (*ready)(void*, DeviceSession&) noexcept) noexcept
    {
        GpuWork result;
        result.session_ = &session;
        result.accepted_ = ticket;
        result.owner_ = {owner, destroy};
        result.ready_ = ready;
        return result;
    }

    RenderResult<bool> GpuWork::advance() noexcept
    {
        if (!ready_) return cxx::unexpected(RenderError{kWrongOwner});
        started_ = true;
        if (advance_)
        {
            auto progress = advance_(owner_.get());
            if (!progress)
            {
                if (!failure_) failure_ = progress.error();
                cancel_(owner_.get()); // accepted chunks and pending CPU source access remain owned
                return false;
            }
            if (!*progress) return false;
        }
        const auto ticket = lastUse();
        if (!ticket)
        {
            if (!failure_) return cxx::unexpected(RenderError{kWrongOwner});
            completed_ = true;
            return true;
        }
        auto* queue = session_->queueFor(*ticket);
        if (!queue) return cxx::unexpected(RenderError{kWrongOwner});
        if (advance_)
            for (std::size_t role = 0; role < before_.size(); ++role)
                if (&session_->queue(static_cast<EQueueRole>(role)) == queue && ticket->serial() <= before_[role])
                    return cxx::unexpected(RenderError{kWrongOwner});
        auto completed = queue->poll();
        if (!completed)
        {
            if (!failure_ || !queue->deviceLost()) return cxx::unexpected(completed.error());
            completed_ = true;
            return true;
        }
        if (*completed < ticket->serial()) return false;
        completed_ = true;
        return true;
    }

    RenderResult<void> GpuWork::publish() noexcept
    {
        if (!completed_ || !ready_) return cxx::unexpected(RenderError{kBusy});
        completed_ = false; // a work owner can publish its business state at most once
        const auto ready = std::exchange(ready_, nullptr);
        if (failure_) return cxx::unexpected(*failure_);
        return ready(owner_.get(), *session_);
    }
}
