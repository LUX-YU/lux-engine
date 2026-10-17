#pragma once

#include <lux/engine/render/data/Payload.hpp>
#include <string_view>
#include <lux/engine/render/protocol/Protocol.hpp>

namespace mesh_sample
{
    // Client implementation has no Native includes or link dependency. Host supplies the service name and a real cold-query CommandId.
    [[nodiscard]] lux::render::RenderPayload runClients(std::string_view channel, lux::render::CommandId list_commands, const char* cooked_fragment);
}
