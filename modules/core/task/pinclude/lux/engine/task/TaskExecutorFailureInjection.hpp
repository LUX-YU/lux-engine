#pragma once

#include <lux/engine/task/visibility.h>

#include <cstdint>

namespace lux::task::detail
{
    enum class ETaskExecutorFailurePoint : std::uint8_t
    {
        ALLOCATION,
        WORKER_CREATION,
    };

    LUX_CORE_TASK_PUBLIC void failNextTaskExecutorOperationForTest(ETaskExecutorFailurePoint point) noexcept;
    [[nodiscard]] LUX_CORE_TASK_PUBLIC bool consumeTaskExecutorFailureForTest(
        ETaskExecutorFailurePoint point
    ) noexcept;
}
