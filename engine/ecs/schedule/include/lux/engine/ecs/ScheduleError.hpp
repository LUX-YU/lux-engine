#pragma once

#include <lux/cxx/compile_time/TypeToken.hpp>

#include <cstdint>

namespace lux::ecs
{
    enum class EScheduleError : std::uint8_t
    {
        EDIT_IN_PROGRESS,
        EXECUTING,
        CLOSING,
        ALLOCATION_FAILURE,
        NULL_SYSTEM,
        TYPE_TOKEN_COLLISION,
        SET_ID_COLLISION,
        PHASE_ORDER_CONTRADICTION,
        DEPENDENCY_CYCLE,
        INVALID_HANDLE,
        INVALID_RELATION,
        SYSTEM_START_FAILED,
        EXECUTION_AFFINITY_MISMATCH,
        SYSTEM_NOT_STOPPED,
        HARD_DEPENDENT_EXISTS,
    };

    struct ScheduleFailure final
    {
        EScheduleError code{EScheduleError::ALLOCATION_FAILURE};
        lux::cxx::TypeToken subject{};
        lux::cxx::TypeToken related{};
    };
} // namespace lux::ecs
