#pragma once
#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>
#include <cstdint>

namespace authoring_consumer
{
    struct LUX_SCRIPT_ABILITY(id=consumer.authoring.counter, name=Counter, display=Counter,
        version=1, receiver=provider_instance) CounterAbility final
    {
        LUX_SCRIPT_COMMAND(id=consumer.authoring.counter.record, display=Record)
        void record(LUX_SCRIPT_PARAM(lifetime=owned_value) std::int32_t value) noexcept;
    };
}
