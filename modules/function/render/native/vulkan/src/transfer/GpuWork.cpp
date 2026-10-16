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
        result.accepted_ready_ = ready;
        return result;
    }

    RenderResult<void> GpuWork::prepareCompletion() noexcept
    {
        return transfer_->prepareCompletion();
    }

    void GpuWork::start(CompletionPost post, void* owner, Transfer::TerminalFn terminal,
        std::shared_ptr<const void> implementation, std::shared_ptr<const void> registration) noexcept
    {
        transfer_->start(post, owner, terminal, std::move(implementation), std::move(registration));
    }

    RenderResult<void> GpuWork::publish() noexcept
    {
        if (!owner_) return cxx::unexpected(RenderError{kWrongOwner});
        if (transfer_)
        {
            if (!ready_ || !transfer_->completed()) return cxx::unexpected(RenderError{kBusy});
            const auto ready = std::exchange(ready_, nullptr);
            if (auto result = transfer_->result(); !result) return result;
            return ready(owner_.get(), *transfer_, *session_);
        }
        if (!accepted_ready_ || !accepted_ || accepted_->owner().completed() < accepted_->serial())
            return cxx::unexpected(RenderError{kBusy});
        if (accepted_->owner().deviceLost()) return cxx::unexpected(nativeError(VK_ERROR_DEVICE_LOST));
        return std::exchange(accepted_ready_, nullptr)(owner_.get(), *session_);
    }
}
