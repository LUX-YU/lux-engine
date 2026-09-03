#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

#include <cstdint>

namespace installed_consumer
{
    struct LUX_SCRIPT_ABILITY(
        id = consumer.inventory.lua,
        name = Inventory,
        display = Inventory,
        version = 1,
        receiver = provider_instance
    ) InventoryAbility
    {
        LUX_SCRIPT_QUERY(
            id = consumer.inventory.lua.count,
            display = Count,
            result_lifetime = owned_value
        )
        std::int32_t count(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t item
        ) noexcept;

        LUX_SCRIPT_ASYNC(
            id = consumer.inventory.lua.count_later,
            display = CountLater,
            result_lifetime = awaitable
        )
        std::int32_t countLater(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t item
        ) noexcept;
    };
} // namespace installed_consumer
