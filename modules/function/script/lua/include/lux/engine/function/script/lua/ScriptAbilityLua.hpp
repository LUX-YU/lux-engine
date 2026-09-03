#pragma once

#include <lux/engine/function/script/ScriptAbility.hpp>

#include <memory>

namespace lux::script::lua
{
    struct ScriptAbilityLuaContribution final
    {
        const ScriptAbilityDescription* description{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return description != nullptr && description->id.isValid() && description->schema_version != 0U &&
                description->schema_hash != 0U;
        }
    };

    template <class Ability>
    [[nodiscard]] constexpr ScriptAbilityLuaContribution makeScriptAbilityLuaContribution() noexcept
    {
        return {std::addressof(ScriptAbilityTraits<Ability>::Description)};
    }
} // namespace lux::script::lua
