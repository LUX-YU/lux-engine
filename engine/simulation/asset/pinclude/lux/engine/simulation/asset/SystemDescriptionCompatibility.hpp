#pragma once

#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/simulation/SystemDescription.hpp>

namespace lux::simulation::asset
{
    template <class CurrentSystem>
    [[nodiscard]] bool matchesCurrentSystemDescription(
        SimulationSystemView asset_system
    ) noexcept
        requires requires { CurrentSystem::Description; }
    {
        constexpr const auto& current = CurrentSystem::Description;
        if (!asset_system || !validSystemDescription(current) ||
            asset_system.type() != systemTypeId(current.canonical_name) ||
            asset_system.version() != current.version ||
            asset_system.configurationSchemaName() !=
                current.configuration_schema_name ||
            asset_system.configurationSchemaVersion() !=
                current.configuration_schema_version ||
            asset_system.capabilityCount() != current.capabilities.size() ||
            asset_system.hookPointCount() != current.hooks.size() ||
            asset_system.eventCount() != current.events.size())
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
            if (actual.id() != expected.id ||
                actual.name() != expected.diagnostic_name ||
                actual.parameterCount() != expected.signature.parameters.size())
            {
                return false;
            }
            for (std::size_t parameter{};
                 parameter < actual.parameterCount(); ++parameter)
            {
                if (actual.parameterAt(parameter) !=
                    expected.signature.parameters[parameter])
                    return false;
            }
        }
        for (std::size_t index{}; index < current.events.size(); ++index)
        {
            const auto actual = asset_system.eventAt(index);
            const auto& expected = current.events[index];
            if (actual.id() != expected.id ||
                actual.name() != expected.diagnostic_name ||
                actual.dispatchHook().id() != expected.dispatch_hook ||
                actual.route() != expected.route ||
                actual.payloadType() != expected.payload_type ||
                actual.payloadSchemaName() != expected.payload_schema_name ||
                actual.payloadSchemaVersion() != expected.payload_schema_version)
            {
                return false;
            }
        }
        return true;
    }
}
