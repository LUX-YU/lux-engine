#pragma once

#include <lux/engine/simulation/scripting/ScriptEndpointBridge.hpp>

namespace lux::simulation::script::test
{
    // The concrete standalone fixture owns this Channel. Production lifetime is managed only by Simulation.
    template <class Route, class Payload>
    std::size_t deliverTypedEndpoint(ScriptEventEndpoint<Route, Payload>& endpoint) noexcept
    {
        const auto descriptor = endpoint.descriptor();
        auto* channel = static_cast<HookChannel<Route, Payload>*>(descriptor.channel_context);
        if (channel == nullptr || !channel->seal())
            return 0U;
        const auto calls = descriptor.consume(descriptor.context);
        channel->reset();
        return calls;
    }

    template <class Endpoint>
    std::size_t deliverEndpoint(Endpoint& endpoint) noexcept
    {
        if constexpr (requires { endpoint.descriptor(); }) return deliverTypedEndpoint(endpoint);
        else return deliverTypedEndpoint(*endpoint);
    }
}
