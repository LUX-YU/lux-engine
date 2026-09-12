#pragma once
#include "Values.hpp"
#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>
struct LUX_SCRIPT_ABILITY(id = consumer.values, name = Values, display = Values, version = 1,
    receiver = provider_instance) ValueAbility
{
    LUX_SCRIPT_QUERY(id = consumer.values.echo, display = Echo, result_lifetime = owned_value)
    Item echo(LUX_SCRIPT_PARAM(lifetime = borrowed_step) const Item& item) noexcept;
};
