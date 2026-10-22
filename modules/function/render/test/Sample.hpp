#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <string_view>

namespace lux::platform::ipc
{
    class IpcRequester;
}

namespace mesh_sample
{
    // Client implementation has no Native includes or link dependency. The application supplies the service name and a
    // real cold-query CommandId.
    void runClients(
        lux::platform::ipc::IpcRequester& main,
        std::string_view channel,
        lux::render::CommandId list_commands,
        const char* cooked_fragment
    );
} // namespace mesh_sample
