#pragma once

#include <lux/engine/render/data/Payload.hpp>
#include <string_view>

namespace mesh_sample
{
    // Client implementation has no Native includes or link dependency. Host supplies only the service name.
    [[nodiscard]] lux::render::RenderPayload runClients(std::string_view channel, const char* cooked_fragment);
}
