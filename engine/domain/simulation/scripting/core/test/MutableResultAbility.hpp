#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

#include <cstdint>

namespace lux::simulation::test
{
    struct LUX_SCRIPT_ABILITY(
        id = lux.test.mutable_result,
        display = MutableResult,
        version = 1,
        receiver = provider_instance
    ) MutableResultAbility final
    {
        LUX_SCRIPT_QUERY(
            id = lux.test.mutable_result.read,
            display = Read,
            result_lifetime = borrowed_step
        )
        std::int32_t& read() noexcept;
    };
} // namespace lux::simulation::test
