#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

#include <cstdint>

namespace installed_consumer
{
    struct LUX_SCRIPT_ABILITY(
        id = lux.installed.tiny,
        name = Tiny,
        display = Tiny,
        version = 1,
        receiver = provider_instance
    ) TinyAbility final
    {
        LUX_SCRIPT_QUERY(id = lux.installed.tiny.read, display = Read, result_lifetime = owned_value)
        std::int32_t read(LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t input) noexcept;
    };
} // namespace installed_consumer
