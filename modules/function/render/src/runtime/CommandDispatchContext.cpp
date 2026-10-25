#include <lux/engine/render/extensions/detail/CommandDispatchContext.hpp>
#include <lux/engine/render/runtime/FrameSubmission.hpp>
#include <lux/engine/render/runtime/ResponseOutbox.hpp>

namespace lux::render::runtime_detail
{
    platform::ipc::IpcMessage& prepareResponse(ResponseOutbox& outbox, ResponseRoute route) noexcept
    {
        return outbox.prepare(route);
    }

    void completeFrameCommand(FrameRecord* frame, RenderError error) noexcept
    {
        if (frame)
        {
            --frame->group_pending;
            const bool replaces_error =
                frame->error.type == 0 || frame->error.type == kCommandCancelled || error.type == kOutcomeUnknown;
            if (error.type != 0 && replaces_error)
            {
                frame->error = error;
            }
        }
    }
} // namespace lux::render::runtime_detail
