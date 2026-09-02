#pragma once

#include <lux/engine/function/script/ScriptAbility.hpp>

#include <cstdint>

namespace lux::simulation::script
{
    struct ScriptApiCapabilityPublication final
    {
        lux::script::ScriptApiContractIdView contract;
        std::uint64_t schema_hash{};
        void* context{};
        const void* dispatch{};
    };

    struct PreparedScriptApiCapability final
    {
        lux::script::ScriptApiContractId contract;
        std::uint64_t schema_hash{};
        void* context{};
        const void* dispatch{};
    };

    [[nodiscard]] inline ScriptApiCapabilityPublication publishScriptAbility(
        const lux::script::ScriptAbilityBinding& binding
    ) noexcept
    {
        if (!binding.valid())
            return {};
        return {
            binding.description->id,
            binding.description->schema_hash,
            binding.context,
            binding.dispatch
        };
    }
} // namespace lux::simulation::script
