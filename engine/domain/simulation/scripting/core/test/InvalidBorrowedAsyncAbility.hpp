#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

#include <cstdint>

namespace lux::simulation::test
{
    struct LUX_SCRIPT_ABILITY(
        id = lux.test.invalid_borrowed_async,
        display = InvalidBorrowedAsync,
        version = 1,
        receiver = provider_instance
    ) InvalidBorrowedAsyncAbility final
    {
        LUX_SCRIPT_ASYNC(
            id = lux.test.invalid_borrowed_async.start,
            display = Start,
            result_lifetime = awaitable
        )
        std::uint64_t start(
            LUX_SCRIPT_PARAM(lifetime = borrowed_step) const std::int32_t& value
        ) noexcept;
    };
} // namespace lux::simulation::test
