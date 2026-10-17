#include "Request.hpp"

namespace lux::render
{
    namespace ipc = platform::ipc;

    RenderRequest::RenderRequest(ipc::ReceivedRequest input, ipc::IpcResponder& responder,
        RequestId request, const CommandHandler* binding) noexcept
        : input_(std::move(input)), responder_(&responder), request_(request), binding_(binding) {}

    RenderResult<void> RenderRequest::prepareResponse(void* response, ipc::IpcMessage& output) noexcept
    {
        if (!active()) return cxx::unexpected(RenderError{kRuntimeState});
        binding_->makeResponse(output, response);
        return {};
    }

    RenderResult<void> RenderRequest::complete(void* response) noexcept
    {
        ipc::IpcMessage output;
        auto prepared = prepareResponse(response, output);
        if (!prepared) return prepared;
        return publishPrepared(output);
    }

    RenderResult<void> RenderRequest::publishPrepared(ipc::IpcMessage& message) noexcept
    {
        return publish(message, ECommandOutcome::SUCCESS);
    }

    RenderResult<void> RenderRequest::publishError(ECommandOutcome outcome, RenderError error) noexcept
    {
        if (!active()) return cxx::unexpected(RenderError{kRuntimeState});
        ipc::IpcMessage message;
        message.emplace<RenderError>(error);
        return publish(message, outcome);
    }

    RenderResult<void> RenderRequest::fail(RenderError error) noexcept { return publishError(ECommandOutcome::FAILED, error); }

    RenderResult<void> RenderRequest::cancel() noexcept { return publishError(ECommandOutcome::CANCELLED, {}); }

    RenderResult<void> RenderRequest::unknown(RenderError error) noexcept
    {
        return publishError(ECommandOutcome::OUTCOME_UNKNOWN, error);
    }

    RenderResult<void> RenderRequest::publish(ipc::IpcMessage& message, ECommandOutcome outcome) noexcept
    {
        message.setMetadata(ReplyHeader{request_, outcome});
        unreplied_.emplace(std::move(message));
        retryDelivery();
        // Completion accepted exactly once, even if delivery must be retried on the owner thread.
        return {};
    }

    void RenderRequest::retryDelivery() noexcept
    {
        if (!awaitingDelivery()) return;
#ifdef LUX_RUNTIME_TEST_SEAM
        const auto header = unreplied_->metadata<ReplyHeader>();
        runtime_detail::test::replyPublication(header, binding_ ? binding_->descriptor()->name.name() : std::string_view{});
#endif
        auto result = responder_->tryReply(input_, *unreplied_);
        if (!result)
        {
            if (result.error() == ipc::EIpcError::WOULD_BLOCK) return;
            if (result.error() != ipc::EIpcError::CLOSED) std::terminate();
            input_.reset(); // Work has ended; connection loss does not claim that its side effects were cancelled.
        }
        unreplied_.reset();
    }

    CommandInvocation RenderRequest::invocation(void* call_state, const CommandCompletionFns& functions) noexcept
    {
        return {binding_->requestValue(input_.message()), call_state, &functions};
    }
}
