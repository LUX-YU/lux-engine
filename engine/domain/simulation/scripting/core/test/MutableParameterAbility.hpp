#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

#include <cstdint>

namespace lux::simulation::test
{
    struct LUX_SCRIPT_ABILITY(
        id = lux.test.mutable_parameter,
        display = MutableParameter,
        version = 1,
        receiver = provider_instance
    ) MutableParameterAbility final
    {
        LUX_SCRIPT_COMMAND(
            id = lux.test.mutable_parameter.write,
            display = Write
        )
        void write(
            LUX_SCRIPT_PARAM(lifetime = borrowed_step) std::int32_t& value
        ) noexcept;
    };
} // namespace lux::simulation::test
