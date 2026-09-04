#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

#include <cstdint>

namespace installed_consumer
{
    struct LUX_SCRIPT_ABILITY(
        id = consumer.static_value,
        name = StaticValue,
        display = StaticValue,
        version = 1,
        receiver = provider_instance
    ) StaticAbility final
    {
        LUX_SCRIPT_QUERY(
            id = consumer.static_value.read,
            display = Read,
            result_lifetime = owned_value
        )
        std::int32_t read(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t input
        ) noexcept;
    };
}
