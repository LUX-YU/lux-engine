#include <lux/engine/render/runtime/RenderServer.hpp>
#include "Wire.hpp"

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

    RenderResult<void> RenderRequest::reply(std::span<const std::byte> bytes, OwnedWireBytes attachment) noexcept
    {
        if (descriptor_)
        {
            if (descriptor_->reply)
            {
                const std::array views{attachment.view()};
                auto valid = validateBoundWire(BorrowedWireView{*descriptor_->reply, bytes,
                    attachment.empty() ? std::span<const std::span<const std::byte>>{} :
                        std::span<const std::span<const std::byte>>(views)}, {reply_capacity_, reply_capacity_});
                if (!valid) return cxx::unexpected(valid.error());
            }
            else if (!bytes.empty() || !attachment.empty()) return cxx::unexpected(RenderError{kProtocolContract});
        }
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
        const bool exceeds_budget = bytes.size() > reply_capacity_ - std::min(reply_capacity_, reply_size) ||
            attachment.size() > reply_capacity_ - std::min(reply_capacity_, reply_size + bytes.size());
        if (exceeds_budget) return cxx::unexpected(RenderError{kRuntimeCapacity});
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
}
