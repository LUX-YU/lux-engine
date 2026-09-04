#pragma once

#include <lux/engine/simulation/scripting/ScriptApiCapability.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

namespace lux::simulation::script
{
    [[nodiscard]] inline bool scriptAbilityValueMatches(
        const lux::script::ScriptAbilityValueDescription& semantic,
        const lux::script::ScriptAbilityValueDescription& runtime
    ) noexcept
    {
        return semantic.type_id == runtime.type_id && semantic.canonical_name == runtime.canonical_name &&
            semantic.pass == runtime.pass && semantic.abi_kind == runtime.abi_kind && semantic.size == runtime.size &&
            semantic.alignment == runtime.alignment && semantic.lifetime == runtime.lifetime;
    }

    [[nodiscard]] inline bool scriptAbilityMethodMatches(
        const lux::script::ScriptAbilityMethodDescription& semantic,
        const lux::script::ScriptAbilityErasedMethodBinding& runtime
    ) noexcept
    {
        const bool has_invalid_shape = semantic.id != runtime.method || semantic.kind != runtime.kind ||
            semantic.parameters.size() != runtime.parameters.size() ||
            semantic.results.size() != runtime.results.size();
        if (has_invalid_shape)
            return false;
        for (std::size_t index{}; index < semantic.parameters.size(); ++index)
        {
            if (!scriptAbilityValueMatches(semantic.parameters[index].value, runtime.parameters[index].value))
                return false;
        }
        for (std::size_t index{}; index < semantic.results.size(); ++index)
        {
            if (!scriptAbilityValueMatches(semantic.results[index], runtime.results[index]))
                return false;
        }
        return semantic.kind == lux::script::EScriptApiMethodKind::ASYNC_OPERATION
            ? runtime.start != nullptr && runtime.invoke == nullptr
            : runtime.invoke != nullptr && runtime.start == nullptr;
    }

    [[nodiscard]] inline bool scriptEventImportMatches(
        const lux_script_event_wait_import_desc& imported,
        const lux::script::ScriptEventSourceDescription& semantic
    ) noexcept
    {
        const auto route = static_cast<std::uint8_t>(semantic.route);
        return imported.system_id == semantic.system_id && imported.event_id == semantic.event_id &&
            imported.route == route && imported.payload_schema_hash == semantic.payload_schema_hash &&
            imported.payload_schema_version == semantic.payload_schema_version && imported.payload.name != nullptr &&
            imported.payload.type_id == semantic.payload.type_id &&
            std::string_view{imported.payload.name} == semantic.payload.canonical_name &&
            imported.payload.kind == semantic.payload.abi_kind && imported.payload.size == semantic.payload.size &&
            imported.payload.align == semantic.payload.alignment && imported.payload.pass == LUX_SCRIPT_PASS_VALUE;
    }

    [[nodiscard]] inline const PreparedScriptApiCapability* findPreparedCapability(
        std::span<const PreparedScriptApiCapability> capabilities,
        const lux::script::ScriptAbilityDescription& requirement
    ) noexcept
    {
        const auto found = std::ranges::find_if(capabilities, [&](const auto& candidate) noexcept {
            return candidate.contract.hash() == requirement.id.hash() &&
                candidate.contract.name() == requirement.id.name();
        });
        if (found == capabilities.end() || found->schema_version != requirement.schema_version ||
            found->schema_hash != requirement.schema_hash)
        {
            return nullptr;
        }
        return std::addressof(*found);
    }
}
