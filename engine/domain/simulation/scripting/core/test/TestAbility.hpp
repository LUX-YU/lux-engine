#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

#include <cstdint>

namespace lux::simulation::test
{
    struct LUX_SCRIPT_ABILITY(
        id = lux.test.value,
        display = TestValue,
        version = 1,
        receiver = provider_instance
    ) TestAbility
    {
        LUX_SCRIPT_QUERY(
            id = lux.test.value.read,
            display = ReadValue,
            result_lifetime = owned_value
        )
        std::int32_t readValue(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t input
        ) noexcept;

        LUX_SCRIPT_COMMAND(
            id = lux.test.value.set,
            display = SetValue
        )
        void setValue(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t value
        ) noexcept;

        LUX_SCRIPT_QUERY(
            id = lux.test.value.identity,
            display = Identity,
            result_lifetime = stable_id
        )
        std::uint64_t identity(
            LUX_SCRIPT_PARAM(lifetime = stable_id) std::uint64_t value
        ) noexcept;

        LUX_SCRIPT_QUERY(
            id = lux.test.value.borrowed,
            display = BorrowedValue,
            result_lifetime = borrowed_step
        )
        const std::int32_t& borrowedValue() noexcept;

        LUX_SCRIPT_ASYNC(
            id = lux.test.value.begin_operation,
            display = BeginOperation,
            result_lifetime = awaitable
        )
        std::uint64_t beginOperation(
            LUX_SCRIPT_PARAM(lifetime = stable_id) std::uint64_t request
        ) noexcept;
    };

    struct LUX_SCRIPT_ABILITY(
        id = lux.test.stateless,
        display = TestStateless,
        version = 1,
        receiver = none
    ) TestStatelessAbility
    {
        LUX_SCRIPT_QUERY(
            id = lux.test.stateless.increment,
            display = Increment,
            result_lifetime = owned_value
        )
        static std::int32_t increment(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::int32_t value
        ) noexcept
        {
            return value + 1;
        }
    };
} // namespace lux::simulation::test
