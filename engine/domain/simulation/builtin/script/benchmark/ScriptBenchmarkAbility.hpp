#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

#include <cstdint>

namespace lux::simulation::script::benchmark
{
    struct LUX_SCRIPT_ABILITY(
        id = lux.benchmark.value,
        name = BenchmarkValue,
        display = BenchmarkValue,
        version = 1,
        receiver = provider_instance
    ) ValueAbility
    {
        LUX_SCRIPT_QUERY(
            id = lux.benchmark.value.read,
            display = Read,
            result_lifetime = owned_value
        )
        std::int32_t read(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t input
        ) noexcept;

        LUX_SCRIPT_COMMAND(
            id = lux.benchmark.value.write,
            display = Write
        )
        void write(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t value
        ) noexcept;
    };
} // namespace lux::simulation::script::benchmark
