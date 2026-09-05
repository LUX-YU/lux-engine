#pragma once
#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>
#include <cstdint>
namespace installed_consumer
{
    struct LUX_SCRIPT_ABILITY(id=consumer.InventoryAbility, name=Inventory, display=Inventory,
        version=1, receiver=provider_instance) InventoryAbility final
    {
        LUX_SCRIPT_QUERY(id=consumer.InventoryAbility.count, result_lifetime=owned_value)
        std::int32_t count() noexcept;
    };
}
