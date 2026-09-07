#pragma once
#include "LuaValueTestTypes.hpp"
#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>
namespace lux::simulation::script::test
{
    struct LUX_SCRIPT_ABILITY(id = lux.test.lua_values, name = Values, display = Values, version = 1,
        receiver = provider_instance) LuaValueTestAbility
    {
        LUX_SCRIPT_QUERY(id = lux.test.lua_values.echo, display = Echo, result_lifetime = owned_value)
        ValuePose echo(LUX_SCRIPT_PARAM(lifetime = borrowed_step) const ValuePose& value) noexcept;
        LUX_SCRIPT_QUERY(id = lux.test.lua_values.angle, display = Angle, result_lifetime = owned_value)
        ValueAngle angle(LUX_SCRIPT_PARAM(lifetime = owned_value) ValueAngle value) noexcept;
        LUX_SCRIPT_QUERY(id = lux.test.lua_values.inspect, display = Inspect, result_lifetime = owned_value)
        std::int32_t inspect(LUX_SCRIPT_PARAM(lifetime = borrowed_step) const ValueConstRecord& value) noexcept;
        LUX_SCRIPT_QUERY(id = lux.test.lua_values.token, display = Token, result_lifetime = owned_value)
        std::int32_t token(LUX_SCRIPT_PARAM(lifetime = borrowed_step) const ValueToken& value,
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t next) noexcept;
        LUX_SCRIPT_QUERY(id = lux.test.lua_values.scalar_probe, display = ScalarProbe, result_lifetime = owned_value)
        std::int32_t scalarProbe(LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t value) noexcept;
        LUX_SCRIPT_QUERY(id = lux.test.lua_values.zero_probe, display = ZeroProbe, result_lifetime = owned_value)
        std::int32_t zeroArgumentProbe() noexcept;
    };
}
