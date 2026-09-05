#pragma once

#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/simulation/SimulationSystemDescription.hpp>

#include <algorithm>

namespace lux::simulation
{
    // Cold, order-independent admission comparison. IDs and semantic contracts are authoritative.
    [[nodiscard]] inline bool matchesSimulationSystemContract(
        SimulationSystemView actual, const SimulationSystemDescription& expected) noexcept
    {
        const bool has_valid_descriptions = actual && validSimulationSystemDescription(expected);
        if (!has_valid_descriptions)
            return false;
        const bool type_mismatch = actual.type().name != expected.type.canonical_name ||
            actual.type().hash != lux::cxx::Fnv1a64::hash(expected.type.canonical_name) ||
            actual.version() != expected.type.version || actual.multiplicity() != expected.type.multiplicity;
        const bool configuration_mismatch =
            actual.configurationSchemaName() != expected.type.configuration_schema_name ||
            actual.configurationSchemaVersion() != expected.type.configuration_schema_version;
        const bool count_mismatch = actual.capabilityCount() != expected.type.capabilities.size() ||
            actual.tasks().size() != expected.tasks.size() || actual.hookPointCount() != expected.hooks.size() ||
            actual.eventCount() != expected.events.size();
        if (type_mismatch || configuration_mismatch || count_mismatch)
            return false;
        for (const auto capability : expected.type.capabilities)
            if (!actual.hasCapability(capability))
                return false;
        for (const auto& stage : expected.tasks)
        {
            if (std::ranges::none_of(actual.tasks(), [&](const auto& task) noexcept { return task.id == stage.id; }))
                return false;
        }
        for (const auto& hook : expected.hooks)
        {
            const auto target = actual.findHookPoint(hook.id);
            if (!target)
                return false;
            const bool contract_mismatch = target.scriptCapable() != hook.script_capable ||
                target.stableResume() != hook.stable_resume || target.contractVersion() != hook.contract_version ||
                target.parameterCount() != hook.signature.parameters.size();
            if (contract_mismatch)
                return false;
            for (std::size_t parameter{}; parameter < target.parameterCount(); ++parameter)
                if (target.parameterAt(parameter) != hook.signature.parameters[parameter])
                    return false;
        }
        for (const auto& event : expected.events)
        {
            const auto target = actual.findEvent(event.id);
            if (!target)
                return false;
            const bool route_mismatch = target.route() != event.route ||
                target.dispatchHook().id() != event.dispatch_hook;
            const bool payload_mismatch = target.payloadType() != event.payload_type ||
                target.payloadSchemaName() != event.payload_schema_name ||
                target.payloadSchemaVersion() != event.payload_schema_version;
            const bool delivery_mismatch = target.ownerReproduction() != event.owner_reproduction;
            if (route_mismatch || payload_mismatch || delivery_mismatch)
                return false;
        }
        return true;
    }
}
