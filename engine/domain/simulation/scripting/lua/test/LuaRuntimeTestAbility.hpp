#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

#include <cstdint>

namespace lux::simulation::script::test
{
    struct LUX_SCRIPT_ABILITY(
        id = lux.test.lua_runtime,
        display = LuaRuntimeTest,
        version = 1,
        receiver = provider_instance
    ) LuaRuntimeTestAbility
    {
        LUX_SCRIPT_QUERY(
            id = lux.test.lua_runtime.read,
            display = ReadValue,
            result_lifetime = owned_value
        )
        std::int32_t readValue(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t input
        ) noexcept;

        LUX_SCRIPT_COMMAND(
            id = lux.test.lua_runtime.write,
            display = WriteValue
        )
        void writeValue(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t value
        ) noexcept;

        LUX_SCRIPT_QUERY(
            id = lux.test.lua_runtime.borrow,
            display = BorrowValue,
            result_lifetime = borrowed_step
        )
        const std::int32_t& borrowValue() noexcept;

        LUX_SCRIPT_ASYNC(
            id = lux.test.lua_runtime.begin,
            display = BeginOperation,
            result_lifetime = awaitable
        )
        std::int32_t beginOperation(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t input
        ) noexcept;
    };
} // namespace lux::simulation::script::test
