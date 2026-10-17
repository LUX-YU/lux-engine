#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>

namespace lux::render::vulkan
{
    RenderResult<void> GpuWork::prepareCompletion() noexcept
    {
        return transfer_->prepareCompletion();
    }

    void GpuWork::start(CompletionPost post, void* owner, Transfer::TerminalFn terminal) noexcept
    {
        transfer_->start(post, owner, terminal);
    }

    RenderResult<void> GpuWork::publish() noexcept
    {
        if (!owner_) return cxx::unexpected(RenderError{kWrongOwner});
        if (!ready_ || !transfer_->completed()) return cxx::unexpected(RenderError{kBusy});
        const auto ready = std::exchange(ready_, nullptr);
        if (auto result = transfer_->result(); !result) return result;
        return ready(owner_.get(), *transfer_, *session_);
    }
}
