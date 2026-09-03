#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

#include <cstdint>

namespace installed_consumer
{
    struct LUX_SCRIPT_ABILITY(
        id = consumer.inventory,
        name = Inventory,
        display = Inventory,
        version = 1,
        receiver = provider_instance
    ) InventoryAbility final
    {
        LUX_SCRIPT_QUERY(
            id = consumer.inventory.count,
            display = Count,
            result_lifetime = owned_value
        )
        std::int32_t count(
            LUX_SCRIPT_PARAM(lifetime = stable_id) std::uint64_t item
        ) noexcept;

        LUX_SCRIPT_COMMAND(
            id = consumer.inventory.set_count,
            display = SetCount
        )
        void setCount(
            LUX_SCRIPT_PARAM(lifetime = stable_id) std::uint64_t item,
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t count
        ) noexcept;

        LUX_SCRIPT_ASYNC(
            id = consumer.inventory.count_later,
            display = CountLater,
            result_lifetime = awaitable
        )
        std::int32_t countLater(
            LUX_SCRIPT_PARAM(lifetime = stable_id) std::uint64_t item
        ) noexcept;
    };
} // namespace installed_consumer
