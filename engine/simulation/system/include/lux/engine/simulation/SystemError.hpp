#pragma once

#include <lux/engine/simulation/SystemId.hpp>

#include <cstdint>

namespace lux::simulation
{
    enum class ESystemError : std::uint8_t
    {
        INVALID_SYSTEM,
        TYPE_COLLISION,
        ALLOCATION_FAILURE,
    };

    struct SystemFailure final
    {
        ESystemError code{ESystemError::INVALID_SYSTEM};
        SystemId system{};
    };
}
