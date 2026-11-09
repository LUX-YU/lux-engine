#include <lux/engine/render/extensions/detail/CommandDispatchContext.hpp>
#include <lux/engine/render/runtime/ResponseOutbox.hpp>

namespace lux::render::runtime_detail
{
    platform::ipc::IpcMessage& prepareResponse(ResponseOutbox& outbox, ResponseRoute route) noexcept
    {
        return outbox.prepare(route);
    }
} // namespace lux::render::runtime_detail
