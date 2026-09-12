#pragma once

#include <lux/engine/function/script/ScriptAbility.hpp>

#include <span>

namespace lux::script::native
{
    struct ScriptAbilityNativeMethodProjection final
    {
        ScriptApiMethodIdView method;
        lux_script_ability_direct_entry_fn entry{};
    };

    struct ScriptAbilityNativeContribution final
    {
        const ScriptAbilityDescription* description{};
        std::span<const ScriptAbilityNativeMethodProjection> methods;
        // C++ cold-binding metadata only; lux_script_prepared_ability_entry and the Native C ABI are unchanged.
        void* expected_context{};
        const void* expected_dispatch{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return description != nullptr && description->id.isValid() &&
                methods.size() == description->methods.size() && description->schema_version != 0U &&
                description->schema_hash != 0U;
        }
    };

    template <class Ability>
    struct ScriptAbilityNativeTraits;

    template <class Ability>
    [[nodiscard]] ScriptAbilityNativeContribution makeScriptAbilityNativeContribution() noexcept
    {
        return {
            std::addressof(ScriptAbilityTraits<Ability>::Description),
            ScriptAbilityNativeTraits<Ability>::Methods
        };
    }
    template <class Ability, class Provider>
        requires ScriptAbilityTraits<Ability>::template ProviderConforms<Provider>
    [[nodiscard]] lux::cxx::expected<ScriptAbilityNativeContribution, EScriptAbilityBindingError>
    makeScriptAbilityNativeContribution(Provider& provider, ScriptAbilityBinding binding) noexcept
    {
        using Traits = ScriptAbilityTraits<Ability>;
        if (!binding.valid())
            return lux::cxx::unexpected(EScriptAbilityBindingError::INVALID_BINDING);
        const bool wrong_provider = binding.context != std::addressof(provider) ||
            binding.dispatch != std::addressof(Traits::template ProviderDispatch<Provider>);
        const bool wrong_contract = binding.description->id != Traits::Description.id ||
            binding.description->schema_hash != Traits::Description.schema_hash ||
            binding.description->schema_version != Traits::Description.schema_version;
        if (wrong_provider || wrong_contract)
            return lux::cxx::unexpected(EScriptAbilityBindingError::CONTRACT_MISMATCH);
        return ScriptAbilityNativeContribution{std::addressof(Traits::Description),
            ScriptAbilityNativeTraits<Ability>::template Entries<Provider>::Methods,
            binding.context, binding.dispatch};
    }
} // namespace lux::script::native
