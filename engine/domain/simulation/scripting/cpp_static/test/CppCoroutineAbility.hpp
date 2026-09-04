#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

#include <cstdint>

namespace lux::simulation::test
{
    struct LUX_SCRIPT_ABILITY(
        id = lux.test.cpp_coroutine,
        name = CppCoroutineTest,
        display = CppCoroutineTest,
        version = 1,
        receiver = provider_instance
    ) CppCoroutineAbility
    {
        LUX_SCRIPT_QUERY(
            id = lux.test.cpp_coroutine.read,
            display = Read,
            result_lifetime = owned_value
        )
        std::int32_t read(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t value
        ) noexcept;

        LUX_SCRIPT_COMMAND(
            id = lux.test.cpp_coroutine.write,
            display = Write
        )
        void write(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t value
        ) noexcept;

        LUX_SCRIPT_QUERY(
            id = lux.test.cpp_coroutine.borrowed,
            display = Borrowed,
            result_lifetime = borrowed_step
        )
        const std::int32_t& borrowed() noexcept;

        LUX_SCRIPT_ASYNC(
            id = lux.test.cpp_coroutine.run,
            display = Run,
            result_lifetime = awaitable
        )
        std::int32_t run(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t value
        ) noexcept;
    };
}
