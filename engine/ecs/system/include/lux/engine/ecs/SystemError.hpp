#pragma once

#include <lux/engine/ecs/SystemId.hpp>

#include <cstdint>

namespace lux::ecs
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
