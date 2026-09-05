#pragma once

#include <lux/engine/simulation/scripting/ScriptEndpointBridge.hpp>

namespace lux::simulation::script::test
{
    // Focused runtime tests only. Production delivery is compiled by Simulation composition.
    template <class Endpoint>
    std::size_t deliverEndpoint(Endpoint& endpoint) noexcept
    {
        const auto descriptor = [&]() noexcept {
            if constexpr (requires { endpoint.descriptor(); })
                return endpoint.descriptor();
            else
                return endpoint->descriptor();
        }();
        if (!descriptor.seal(descriptor.context))
            return 0U;
        const auto calls = descriptor.consume(descriptor.context);
        descriptor.reset(descriptor.context);
        return calls;
    }
}
