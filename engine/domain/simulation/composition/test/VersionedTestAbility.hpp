#pragma once
#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>
#include <cstdint>

namespace versioned_test
{
    struct LUX_SCRIPT_ABILITY(id=lux.test.versioned, name=Versioned, display=Versioned,
        version=2, receiver=provider_instance) Ability
    {
        LUX_SCRIPT_QUERY(id=lux.test.versioned.read, result_lifetime=owned_value)
        std::int32_t read(LUX_SCRIPT_PARAM(lifetime=owned_value) std::int32_t value) noexcept;
    };
}
