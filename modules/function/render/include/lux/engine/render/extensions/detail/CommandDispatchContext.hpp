#pragma once

#include <lux/cxx/execution/ManualContext.hpp>
#include <lux/engine/render/extensions/detail/ResponseRoute.hpp>
#include <lux/engine/render/extensions/resource/ResourceCommandUse.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render::runtime_detail
{
    class ResponseOutbox;

    // Template dispatch implementation, not a Handler API. Borrowed for one invocation only.
    // A Sender captures its destination and frame association by value, never this stack object.
    struct CommandDispatchContext final
    {
        platform::ipc::IpcMessage& request;
        ResponseRoute response_route;
        ResponseOutbox& outbox;
        stdexec::counting_scope& scope;
        cxx::execution::ManualContext::Scheduler server_delivery;
        // Qualified by the Server before invoking a typed professional method. Never captured as context.
        ResourceCommandUse resource;
    };

    LUX_RENDER_PUBLIC platform::ipc::IpcMessage& prepareResponse(ResponseOutbox&, ResponseRoute) noexcept;
} // namespace lux::render::runtime_detail
