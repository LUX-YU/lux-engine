#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

#include <cstdint>

namespace lux::simulation::test
{
    struct LUX_SCRIPT_ABILITY(
        id = lux.test.duplicate_method,
        display = DuplicateMethod,
        version = 1,
        receiver = provider_instance
    ) DuplicateMethodIdAbility final
    {
        LUX_SCRIPT_QUERY(
            id = lux.test.duplicate_method.read,
            display = FirstRead,
            result_lifetime = owned_value
        )
        std::int32_t firstRead() noexcept;

        LUX_SCRIPT_QUERY(
            id = lux.test.duplicate_method.read,
            display = SecondRead,
            result_lifetime = owned_value
        )
        std::int32_t secondRead() noexcept;
    };
} // namespace lux::simulation::test
