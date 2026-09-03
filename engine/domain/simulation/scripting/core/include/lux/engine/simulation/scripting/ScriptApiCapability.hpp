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
        std::uint32_t schema_version{1U};
        std::span<const lux::script::ScriptAbilityErasedMethodBinding> methods;
    };

    struct PreparedScriptApiCapability final
    {
        lux::script::ScriptApiContractId contract;
        std::uint64_t schema_hash{};
        void* context{};
        const void* dispatch{};
        std::uint32_t schema_version{1U};
        std::span<const lux::script::ScriptAbilityErasedMethodBinding> methods;
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
            binding.dispatch,
            binding.description->schema_version,
            binding.erased_methods
        };
    }
} // namespace lux::simulation::script
