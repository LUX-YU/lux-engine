#include "CommandDispatchContext.hpp"

namespace lux::render::runtime_detail
{
    platform::ipc::IpcMessage& requestMessage(CommandDispatchContext& context) noexcept
    {
        return context.request_message_;
    }

    platform::ipc::IpcMessage& prepareResponse(CommandDispatchContext& context) noexcept
    {
        return context.response_outbox_.prepare(context.response_route_);
    }

    PendingOperation& createPendingOperation(CommandDispatchContext& context) noexcept
    {
        const auto id = context.pending_operations_.emplace();
        auto& slot = *context.pending_operations_.find(id);
        slot.id = id;
        slot.response_route_ = context.response_route_;
        slot.events = &context.completion_queue_;
        slot.publishers = &context.publishers_;
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
        test::completionRegistered(id, slot.response_route_.request_id);
#endif
        return slot;
    }

    platform::ipc::IpcMessage& operationResult(PendingOperation& slot) noexcept
    {
        return slot.result_message_;
    }

    stdexec::inplace_stop_token operationStopToken(PendingOperation& slot) noexcept
    {
        return slot.stop.get_token();
    }

    execution::CompletionPost<RenderError> operationPost(PendingOperation& slot) noexcept
    {
        return slot.post();
    }

    void ownOperation(PendingOperation& slot, void* operation, void (*destroy)(void*) noexcept) noexcept
    {
        slot.operation_state_ = operation;
        slot.destroy_operation_ = destroy;
    }

    void publishOperation(PendingOperation& slot) noexcept
    {
        slot.post()(nullptr, nullptr, {});
    }
} // namespace lux::render::runtime_detail
