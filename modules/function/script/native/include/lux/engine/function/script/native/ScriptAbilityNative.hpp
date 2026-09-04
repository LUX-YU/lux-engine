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
} // namespace lux::script::native
