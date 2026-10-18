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

    CommandCompletion& prepareCompletion(CommandDispatchContext& context) noexcept
    {
        auto* record = new CommandCompletion;
        record->response_route_ = context.response_route_;
        record->loop = &context.render_loop_;
        record->outbox = &context.response_outbox_;
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
        record->measurement = test::completionRegistered(record, record->response_route_.request_id);
#endif
        return *record;
    }

    platform::ipc::IpcMessage& completionResult(CommandCompletion& record) noexcept
    {
        return record.result_message_;
    }

    execution::CompletionPost<RenderError> completionPost(CommandCompletion& record) noexcept
    {
        return record.post();
    }

    stdexec::counting_scope& commandScope(CommandDispatchContext& context) noexcept
    {
        return context.scope_;
    }

    void publishCompletion(CommandCompletion& record) noexcept
    {
        record.post()(nullptr, nullptr, {});
    }
} // namespace lux::render::runtime_detail
