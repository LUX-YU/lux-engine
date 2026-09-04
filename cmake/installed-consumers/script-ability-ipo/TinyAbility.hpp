#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

#include <cstdint>

namespace installed_consumer
{
    struct LUX_SCRIPT_ABILITY(
        "lux.installed.tiny",
        "provider_instance",
        name = "Tiny",
        display = "Tiny"
    ) TinyAbility final
    {
        LUX_SCRIPT_QUERY("lux.installed.tiny.read")
        std::int32_t read(LUX_SCRIPT_PARAM("owned_value") std::int32_t input) noexcept;
    };
} // namespace installed_consumer
