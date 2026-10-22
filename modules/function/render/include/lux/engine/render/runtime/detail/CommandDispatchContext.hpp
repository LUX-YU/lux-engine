#pragma once

#include <lux/engine/core/execution/RunLoop.hpp>
#include <lux/engine/render/runtime/detail/ResponseRoute.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render::runtime_detail
{
    class ResponseOutbox;
    struct FrameRecord;

    // Template dispatch implementation, not a Handler API. Borrowed for one invocation only.
    // A Sender captures its destination and frame association by value, never this stack object.
    struct CommandDispatchContext final
    {
        platform::ipc::IpcMessage& request;
        ResponseRoute response_route;
        ResponseOutbox& outbox;
        stdexec::counting_scope& scope;
        execution::RunLoop& render_loop;
        FrameRecord* frame = nullptr;
    };

    LUX_RENDER_PUBLIC platform::ipc::IpcMessage& prepareResponse(ResponseOutbox&, ResponseRoute) noexcept;
    LUX_RENDER_PUBLIC void completeFrameCommand(FrameRecord*, RenderError) noexcept;
} // namespace lux::render::runtime_detail
