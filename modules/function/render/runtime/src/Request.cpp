#include <lux/engine/render/runtime/RenderServer.hpp>
#include "Wire.hpp"
#include "Deferred.hpp"

namespace lux::render
{
    namespace ipc = platform::ipc;
    using namespace runtime_detail;

    RenderRequest::RenderRequest(ipc::ReceivedRequest input, ipc::IpcResponder& responder,
        SessionIdentity session, RequestId request, std::size_t capacity, const OperationDescriptor* descriptor) noexcept
        : input_(std::move(input)), responder_(&responder), session_(session), request_(request),
          reply_capacity_(capacity), descriptor_(descriptor) {}

    BorrowedWireView RenderRequest::payload() const noexcept
    {
        return {descriptor_->payload, input_.message().bytes().subspan(request_size), input_.message().attachments()};
    }

    RenderResult<void> RenderRequest::validateReply(std::span<const std::byte> bytes,
        const OwnedWireBytes& attachment) const noexcept
    {
        const bool exceeds_budget = bytes.size() > reply_capacity_ - std::min(reply_capacity_, reply_size) ||
            attachment.size() > reply_capacity_ - std::min(reply_capacity_, reply_size + bytes.size());
        if (exceeds_budget) return cxx::unexpected(RenderError{kRuntimeCapacity});
        if (!descriptor_) return {};
        if (descriptor_->reply)
        {
            const std::array views{attachment.view()};
            return validateBoundWire(BorrowedWireView{*descriptor_->reply, bytes,
                attachment.empty() ? std::span<const std::span<const std::byte>>{} :
                    std::span<const std::span<const std::byte>>(views)}, {reply_capacity_, reply_capacity_});
        }
        if (!bytes.empty() || !attachment.empty()) return cxx::unexpected(RenderError{kProtocolContract});
        return {};
    }

    RenderResult<void> RenderRequest::reply(std::span<const std::byte> bytes, OwnedWireBytes attachment) noexcept
    {
        if (descriptor_ && descriptor_->completion == ECompletionContract::RESOURCE_GPU_READY)
            return cxx::unexpected(RenderError{kProtocolContract});
        if (auto valid = validateReply(bytes, attachment); !valid) return valid;
        return publish(EOperationOutcome::SUCCESS, {}, bytes, std::move(attachment));
    }

    RenderResult<void> RenderRequest::fail(RenderError error) noexcept
    {
        return publish(EOperationOutcome::FAILED, error, {}, {});
    }

    RenderResult<void> RenderRequest::cancel() noexcept
    {
        return publish(EOperationOutcome::CANCELLED, {}, {}, {});
    }

    RenderResult<void> RenderRequest::publish(EOperationOutcome outcome, RenderError error,
        std::span<const std::byte> bytes, OwnedWireBytes attachment) noexcept
    {
        if (!active() || unreplied_) return cxx::unexpected(RenderError{kRuntimeState});
        const auto total = reply_size + bytes.size();
        ipc::IpcMessage message;
        if (total <= ipc::IpcMessage::inline_capacity && attachment.empty())
        {
            std::array<std::byte, 128> storage;
            cxx::BinarySpanWriter writer(storage);
            writeReply(writer, {session_, {request_, outcome, error}}, static_cast<std::uint32_t>(bytes.size()));
            writer.writeBytes(bytes);
            if (!message.tryAssignInline(std::span(storage).first(total))) std::terminate();
        }
        else
        {
            struct Output final
            {
                std::vector<std::byte> header;
                OwnedWireBytes data;
                std::array<std::span<const std::byte>, 1> views;
            };
            auto owner = std::make_unique<Output>();
            owner->header.resize(total);
            owner->data = std::move(attachment);
            owner->views[0] = owner->data.view();
            cxx::BinarySpanWriter writer(owner->header);
            writeReply(writer, {session_, {request_, outcome, error}}, static_cast<std::uint32_t>(bytes.size()));
            writer.writeBytes(bytes);
            const auto views = owner->data.empty() ? std::span<const std::span<const std::byte>>{} :
                std::span<const std::span<const std::byte>>(owner->views);
            if (!message.tryAdopt(owner, owner->header, views)) std::terminate();
        }
        if (!responder_->tryReply(input_, message))
        {
            unreplied_ = std::make_unique<ipc::IpcMessage>(std::move(message));
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        return {};
    }

    RenderResult<GpuCompletionReservation> OperationContext::reserveGpuCompletion(
        std::span<const std::byte> reply, OwnedWireBytes attachment) noexcept
    {
        auto& request = *request_;
        const bool is_invalid_contract = reserved_ || !request.active() || !request.descriptor_ ||
            request.descriptor_->completion != ECompletionContract::RESOURCE_GPU_READY;
        if (is_invalid_contract) return cxx::unexpected(RenderError{kProtocolContract});
        if (reply.size() > 128) return cxx::unexpected(RenderError{kRuntimeCapacity});
        if (auto valid = request.validateReply(reply, attachment); !valid) return cxx::unexpected(valid.error());
        for (auto& slot : slots_)
        {
            if (!std::holds_alternative<std::monostate>(slot.condition)) continue;
            auto& gpu = slot.condition.emplace<GpuCompletion>();
            gpu.submitted_before_reserve = native_->queue().submitted();
            std::copy(reply.begin(), reply.end(), gpu.reply.begin());
            gpu.size = reply.size();
            gpu.attachment = std::move(attachment);
            reserved_ = true;
            return GpuCompletionReservation{*native_, slot, request};
        }
        return cxx::unexpected(RenderError{kRuntimeCapacity});
    }

    GpuCompletionReservation::~GpuCompletionReservation() noexcept
    {
        if (slot_) slot_->condition.emplace<std::monostate>();
    }

    GpuCompletionReservation::GpuCompletionReservation(GpuCompletionReservation&& other) noexcept
        : native_(other.native_), slot_(std::exchange(other.slot_, nullptr)), request_(other.request_) {}

    RenderResult<void> GpuCompletionReservation::commitOwned(const vulkan::SubmissionTicket& ticket, void* owner,
        void (*destroy)(void*) noexcept, RenderResult<void> (*ready)(void*, vulkan::DeviceSession&) noexcept) noexcept
    {
        if (!slot_ || !request_->active()) return cxx::unexpected(RenderError{kRuntimeState});
        auto& gpu = std::get<GpuCompletion>(slot_->condition);
        const bool is_invalid_ticket = !native_->queue().owns(ticket) ||
            ticket.serial() <= gpu.submitted_before_reserve;
        if (is_invalid_ticket) return cxx::unexpected(RenderError{kProtocolContract});
        gpu.ticket = ticket;
        gpu.owner = {owner, destroy};
        gpu.ready = ready;
        slot_->request.emplace(std::move(*request_));
        slot_ = nullptr;
        return {};
    }

}
