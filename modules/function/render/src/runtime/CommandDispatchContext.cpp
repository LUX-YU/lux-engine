#include <lux/engine/render/runtime/CommandDispatchContext.hpp>

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

    ResponseDelivery& prepareCompletion(CommandDispatchContext& context) noexcept
    {
        auto* record = new ResponseDelivery;
        record->response_route_ = context.response_route_;
        record->loop = &context.render_loop_;
        record->outbox = &context.response_outbox_;
        return *record;
    }

    stdexec::counting_scope& commandScope(CommandDispatchContext& context) noexcept
    {
        return context.scope_;
    }

    execution::RunLoop::Scheduler renderScheduler(CommandDispatchContext& context) noexcept
    {
        return context.render_loop_.scheduler();
    }

} // namespace lux::render::runtime_detail
