#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

#include <cstdint>

namespace lux::simulation::script
{
    enum class EScriptDelayStatus : std::int32_t
    {
        INVALID_DURATION = 1,
        DURATION_OVERFLOW = 2,
        CAPACITY_EXCEEDED = 3,
        STOPPING = 4,
        ALLOCATION_FAILURE = 5,
        TIMER_FAILURE = 6,
    };

    struct LUX_SCRIPT_ABILITY(
        id = lux.simulation.delay,
        display = Delay,
        version = 1,
        receiver = provider_instance
    ) DelayAbility
    {
        LUX_SCRIPT_ASYNC(
            id = lux.simulation.delay.next_step,
            display = NextStep,
            result_lifetime = awaitable
        )
        void nextStep() noexcept;
    };
} // namespace lux::simulation::script
