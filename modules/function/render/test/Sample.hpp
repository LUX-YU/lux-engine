#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <string_view>

namespace mesh_sample
{
    // Client implementation has no Native includes or link dependency. The application supplies the service name and a
    // real cold-query CommandId.
    void runClients(std::string_view channel, lux::render::CommandId list_commands, const char* cooked_fragment);
} // namespace mesh_sample
