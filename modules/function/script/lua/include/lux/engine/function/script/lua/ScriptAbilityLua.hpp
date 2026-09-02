#pragma once

#include <lux/engine/function/script/ScriptAbility.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>

struct lua_State;

namespace lux::script::lua
{
    enum class EScriptAbilityLuaError : std::uint8_t
    {
        INVALID_BINDING,
        REGISTRATION_FAILURE,
    };

    template <class Ability>
    struct ScriptAbilityLuaProjection;

    template <class Ability>
    [[nodiscard]] lux::cxx::expected<void, EScriptAbilityLuaError> projectScriptAbility(
        lua_State& state,
        ScriptAbilityBinding binding
    ) noexcept
    {
        return ScriptAbilityLuaProjection<Ability>::bind(state, binding);
    }
} // namespace lux::script::lua
