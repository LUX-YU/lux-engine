#include <lux/engine/render/native/vulkan/CommandCompletion.hpp>

#include <utility>

namespace lux::render::vulkan
{
    RenderResult<GpuCompletionReservation> GpuCompletionReservation::reserve(
        CommandInvocation& invocation, SubmissionQueue& queue, void* response
    ) noexcept
    {
        const auto* access = static_cast<const GpuCompletionAccess*>(invocation.completion_extension);
        if (!access) return cxx::unexpected(RenderError{kProtocolContract});
        auto reserved = access->functions->reserve(access->owner, queue, response);
        if (!reserved) return cxx::unexpected(reserved.error());
        return GpuCompletionReservation{*reserved, *access->functions};
    }

    GpuCompletionReservation::~GpuCompletionReservation() noexcept
    {
        if (slot_) functions_->rollback(slot_);
    }

    GpuCompletionReservation::GpuCompletionReservation(GpuCompletionReservation&& other) noexcept
        : slot_(std::exchange(other.slot_, nullptr)), functions_(other.functions_) {}

    RenderResult<void> GpuCompletionReservation::setReply(void* response) noexcept
    {
        if (!slot_) return cxx::unexpected(RenderError{kProtocolContract});
        return functions_->setReply(slot_, response);
    }

    RenderResult<void> GpuCompletionReservation::commitWork(GpuWork& work) noexcept
    {
        if (!slot_) return cxx::unexpected(RenderError{kProtocolContract});
        auto committed = functions_->commitWork(slot_, work);
        if (committed) slot_ = nullptr;
        return committed;
    }

    RenderResult<void> GpuCompletionReservation::commitOwned(
        const SubmissionTicket& ticket, void* owner, void (*destroy)(void*) noexcept,
        RenderResult<void> (*ready)(void*, DeviceSession&) noexcept
    ) noexcept
    {
        if (!slot_) return cxx::unexpected(RenderError{kProtocolContract});
        auto committed = functions_->commitOwned(slot_, ticket, owner, destroy, ready);
        if (committed) slot_ = nullptr;
        return committed;
    }
}
