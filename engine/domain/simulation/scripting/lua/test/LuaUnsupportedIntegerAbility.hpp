#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

#include <cstdint>

namespace lux::simulation::script::test
{
    struct LUX_SCRIPT_ABILITY(
        id = lux.test.lua_unsupported_integer,
        name = UnsupportedInteger,
        display = Unsupported Integer,
        version = 1,
        receiver = provider_instance
    ) LuaUnsupportedIntegerAbility
    {
        LUX_SCRIPT_QUERY(
            id = lux.test.lua_unsupported_integer.echo,
            display = Echo,
            result_lifetime = owned_value
        )
        std::int64_t echo(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int64_t value
        ) noexcept;
    };
} // namespace lux::simulation::script::test
