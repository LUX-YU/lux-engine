#pragma once

#include <lux/cxx/execution/ManualContext.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/extensions/detail/ResponseRoute.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    class ResourceRegistry;
}

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
        ResourceRegistry& resources;
        ResourceInstanceId resource_id;
        void* resource = nullptr;
    };

    LUX_RENDER_PUBLIC void finishResourceCommand(ResourceRegistry&, ResourceInstanceId) noexcept;
    LUX_RENDER_PUBLIC platform::ipc::IpcMessage& prepareResponse(ResponseOutbox&, ResponseRoute) noexcept;
} // namespace lux::render::runtime_detail
