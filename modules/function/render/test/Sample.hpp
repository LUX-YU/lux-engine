#pragma once

#include <array>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <string_view>
#include <tuple>

struct RenderProbeCapability final
{
    using PrerequisiteCapabilities = std::tuple<>;
    using Commands = std::tuple<>;
    using PerFrameData = std::tuple<>;
};

namespace lux::platform::ipc
{
    class IpcRequester;
}

namespace mesh_sample
{
    // Each caller owns a RenderClient and drives its replies; no Engine or Native headers are required.
    void runClients(
        lux::platform::ipc::IpcRequester main,
        std::string_view channel,
        const char* cooked_fragment,
        std::array<lux::render::CapabilityImplementationId, 2> implementations
    );
} // namespace mesh_sample
