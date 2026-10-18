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
        return *record;
    }

    platform::ipc::IpcMessage& completionResult(CommandCompletion& record) noexcept
    {
        return record.result_message_;
    }

    stdexec::counting_scope& commandScope(CommandDispatchContext& context) noexcept
    {
        return context.scope_;
    }

    execution::RunLoop::Scheduler renderScheduler(CommandDispatchContext& context) noexcept
    {
        return context.render_loop_.scheduler();
    }

    void publishCompletion(CommandCompletion& record) noexcept
    {
        auto* loop = record.loop;
        loop->post(record);
        // No record access after publication. RunLoop protects the wakeup/publisher tail.
    }
} // namespace lux::render::runtime_detail
