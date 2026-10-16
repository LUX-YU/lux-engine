#include "Request.hpp"


namespace lux::render
{
    namespace ipc = platform::ipc;
    using namespace runtime_detail;

    RenderRequest::RenderRequest(ipc::ReceivedRequest input, ipc::IpcResponder& responder,
        SessionIdentity session, RequestId request, std::size_t capacity, const CommandDescriptor* descriptor,
        const CommandTransport* transport, const ipc::IpcBoundType<ValueEnvelope<RenderError>>& failure_type,
        std::shared_ptr<const void> code_pin) noexcept
        : code_pin_(std::move(code_pin)), input_(std::move(input)), responder_(&responder), session_(session),
          request_(request), reply_capacity_(capacity), descriptor_(descriptor), transport_(transport),
          failure_type_(&failure_type) {}

    BudgetLease RenderRequest::splitBudget(std::size_t bytes) noexcept
    {
        if (!transport_) return {}; // An untrusted raw peer has no Client budget credential.
        // ReceivedRequest owns a non-const message; only its public inspection interface is const.
        auto& input = const_cast<ipc::IpcMessage&>(input_.message());
        return transport_->request_budget(input).split(bytes);
    }

    RenderResult<std::size_t> RenderRequest::prepareResponse(void* response, ipc::IpcMessage& output) noexcept
    {
        if (!active() || unreplied_) return cxx::unexpected(RenderError{kRuntimeState});
        auto bytes = transport_->response_bytes(response);
        if (!bytes) return cxx::unexpected(bytes.error());
        if (*bytes > reply_capacity_) return cxx::unexpected(RenderError{kRuntimeCapacity});
        const auto overhead = transport_->response->size() - descriptor_->response.size;
        transport_->package_response(output, response, {}, code_pin_);
        return *bytes + overhead; // Client checked this bounded sum before moving the input.
    }

    RenderResult<void> RenderRequest::complete(void* response) noexcept
    {
        if (gpuReady()) return cxx::unexpected(RenderError{kProtocolContract});
        ipc::IpcMessage output;
        auto bytes = prepareResponse(response, output);
        if (!bytes) return cxx::unexpected(bytes.error());
        return publishPrepared(output, *bytes);
    }

    RenderResult<void> RenderRequest::publishPrepared(ipc::IpcMessage& message, std::size_t charge) noexcept
    {
        transport_->response_budget(message) = splitBudget(charge);
        return publish(message, ECommandOutcome::SUCCESS);
    }

    RenderResult<void> RenderRequest::publishError(ECommandOutcome outcome, RenderError error) noexcept
    {
        if (!active() || unreplied_) return cxx::unexpected(RenderError{kRuntimeState});
        ipc::IpcMessage message;
        message.emplace(*failure_type_, splitBudget(sizeof(ValueEnvelope<RenderError>)), std::move(error));
        return publish(message, outcome);
    }

    RenderResult<void> RenderRequest::fail(RenderError error) noexcept
    {
        return publishError(ECommandOutcome::FAILED, error);
    }

    RenderResult<void> RenderRequest::cancel() noexcept
    {
        return publishError(ECommandOutcome::CANCELLED, {});
    }

    RenderResult<void> RenderRequest::unknown() noexcept
    {
        return publishError(ECommandOutcome::OUTCOME_UNKNOWN, {});
    }

    RenderResult<void> RenderRequest::publish(ipc::IpcMessage& message, ECommandOutcome outcome) noexcept
    {
        message.setMetadata(ReplyMetadata{session_, request_, outcome});
        if (!responder_->tryReply(input_, message))
        {
            unreplied_ = std::make_unique<ipc::IpcMessage>(std::move(message));
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        return {};
    }

    CommandInvocation RenderRequest::invocation(
        void* call_state, const CommandCompletionFns& functions, void* extension
    ) noexcept
    {
        return {transport_->request_value(input_.message()), call_state, &functions, extension};
    }
}
