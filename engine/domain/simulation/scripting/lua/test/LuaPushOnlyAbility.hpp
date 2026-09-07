#pragma once
#include "LuaValueTestTypes.hpp"
#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>
namespace lux::simulation::script::test
{
    struct LUX_SCRIPT_ABILITY(id = lux.test.lua_push_only, name = PushOnly, display = PushOnly, version = 1,
        receiver = provider_instance) LuaPushOnlyAbility
    {
        LUX_SCRIPT_QUERY(id = lux.test.lua_push_only.read, display = Read, result_lifetime = owned_value)
        std::int32_t read(LUX_SCRIPT_PARAM(lifetime = owned_value) ValuePushOnly value) noexcept;
    };
}
