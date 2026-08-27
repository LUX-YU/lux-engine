#pragma once

#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/simulation/SystemDescription.hpp>

namespace lux::simulation::asset
{
    template <class CurrentSystem>
    [[nodiscard]] bool matchesCurrentSystemDescription(SimulationSystemView asset_system) noexcept
        requires requires { CurrentSystem::Description; }
    {
        constexpr const auto& current = CurrentSystem::Description;
        const bool is_invalid_description = !asset_system || !validSystemDescription(current);
        const bool is_invalid_identity = !asset_system ||
            asset_system.type() != systemTypeId(current.canonical_name) ||
            asset_system.version() != current.version;
        const bool is_invalid_configuration = !asset_system ||
            asset_system.configurationSchemaName() != current.configuration_schema_name ||
            asset_system.configurationSchemaVersion() != current.configuration_schema_version;
        const bool is_invalid_counts = !asset_system ||
            asset_system.capabilityCount() != current.capabilities.size() ||
            asset_system.hookPointCount() != current.hooks.size() ||
            asset_system.eventCount() != current.events.size();
        const bool is_invalid_system = is_invalid_description || is_invalid_identity ||
            is_invalid_configuration || is_invalid_counts;

        if (is_invalid_system)
        {
            return false;
        }
        for (std::size_t index{}; index < current.capabilities.size(); ++index)
        {
            if (asset_system.capabilityAt(index) != current.capabilities[index])
                return false;
        }
        for (std::size_t index{}; index < current.hooks.size(); ++index)
        {
            const auto actual = asset_system.hookPointAt(index);
            const auto& expected = current.hooks[index];
            const bool is_invalid_identity = actual.name() != expected.name ||
                actual.cardinality() != expected.cardinality;
            const bool is_invalid_signature =
                actual.parameterCount() != expected.signature.parameters.size() ||
                actual.returnCount() != expected.signature.returns.size();
            const bool is_invalid_hook = is_invalid_identity || is_invalid_signature;
            if (is_invalid_hook)
            {
                return false;
            }
            for (std::size_t parameter{}; parameter < actual.parameterCount(); ++parameter)
            {
                if (actual.parameterAt(parameter) != expected.signature.parameters[parameter])
                    return false;
            }
            for (std::size_t result{}; result < actual.returnCount(); ++result)
            {
                if (actual.returnAt(result) != expected.signature.returns[result])
                    return false;
            }
        }
        for (std::size_t index{}; index < current.events.size(); ++index)
        {
            const auto actual = asset_system.eventAt(index);
            const auto& expected = current.events[index];
            const bool is_invalid_identity = actual.name() != expected.name ||
                actual.dispatchHook().name() != expected.dispatch_hook || actual.target() != expected.target;
            const bool is_invalid_payload =
                actual.payloadSchemaName() != expected.payload_schema_name ||
                actual.payloadSchemaVersion() != expected.payload_schema_version;
            const bool is_invalid_event = is_invalid_identity || is_invalid_payload;
            if (is_invalid_event)
            {
                return false;
            }
        }
        return true;
    }
}
