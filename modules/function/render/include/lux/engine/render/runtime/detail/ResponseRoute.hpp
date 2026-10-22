#pragma once

#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Command.hpp>

namespace lux::render::runtime_detail
{
    // Non-owning destination retained by an asynchronous command until its terminal handoff.
    struct ResponseRoute final
    {
        platform::ipc::RequesterId requester_id;
        RequestId request_id;
    };
} // namespace lux::render::runtime_detail
