#pragma once

#include <lux/engine/ecs/SystemId.hpp>

#include <cstdint>

namespace lux::ecs
{
    enum class ESystemError : std::uint8_t
    {
        INVALID_SYSTEM,
        DUPLICATE_RELATION,
        RELATION_CYCLE,
        TYPE_COLLISION,
        INVALID_ACCESS,
        EXECUTION_AFFINITY_MISMATCH,
        START_FAILED,
        STALE_COMPILATION,
        WORLD_BUSY,
        ALLOCATION_FAILURE
    };

    struct SystemFailure final
    {
        ESystemError code{ESystemError::INVALID_SYSTEM};
        SystemId system{};
        SystemId related{};
    };
}
