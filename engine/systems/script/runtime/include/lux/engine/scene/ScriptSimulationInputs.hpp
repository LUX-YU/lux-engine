#pragma once

#include <lux/engine/simulation/scripting/ScriptApiCapability.hpp>
#include <lux/engine/simulation/scripting/ScriptEndpointBridge.hpp>
#include <vector>

namespace lux::scene
{
    // Temporary cold assembly values. The professional assembler borrows real installed Systems;
    // ScriptSystem consumes their descriptors. Simulation owns neither a second directory nor a VM.
    struct ScriptSimulationInputs final
    {
        std::vector<simulation::script::ScriptApiCapabilityPublication> capabilities;
        std::vector<simulation::script::ScriptHookEndpointDescriptor> hooks;
        std::vector<simulation::script::ScriptEventEndpointDescriptor> events;
    };
} // namespace lux::scene
