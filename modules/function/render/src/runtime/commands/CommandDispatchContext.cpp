#include <lux/engine/render/runtime/commands/detail/CommandDispatchContext.hpp>
#include <lux/engine/render/runtime/commands/ResponseOutbox.hpp>

namespace lux::render::runtime_detail
{
    platform::ipc::IpcMessage& prepareResponse(ResponseOutbox& outbox, ResponseRoute route) noexcept
    {
        return outbox.prepare(route);
    }
} // namespace lux::render::runtime_detail
