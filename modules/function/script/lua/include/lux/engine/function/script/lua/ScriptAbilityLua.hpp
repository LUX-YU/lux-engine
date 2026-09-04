#pragma once

#include <lux/engine/function/script/ScriptAbility.hpp>

#include <memory>
#include <span>

struct lua_State;

namespace lux::script::lua
{
    struct ScriptAbilityLuaMethodProjection final
    {
        ScriptApiMethodIdView method;
        int (*entry)(lua_State*) noexcept{};
    };

    struct ScriptAbilityLuaContribution final
    {
        const ScriptAbilityDescription* description{};
        std::span<const ScriptAbilityLuaMethodProjection> methods;

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return description != nullptr && description->id.isValid() && methods.size() == description->methods.size() &&
                scriptAbilityCodeNameValid(description->name) && description->schema_version != 0U &&
                description->schema_hash != 0U;
        }
    };

    template <class Ability>
    struct ScriptAbilityLuaTraits;

    template <class Ability>
    [[nodiscard]] constexpr ScriptAbilityLuaContribution makeScriptAbilityLuaContribution() noexcept
    {
        return {
            std::addressof(ScriptAbilityTraits<Ability>::Description),
            ScriptAbilityLuaTraits<Ability>::Methods
        };
    }
} // namespace lux::script::lua
