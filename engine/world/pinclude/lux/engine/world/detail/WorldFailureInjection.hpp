#pragma once

#include <lux/engine/world/visibility.h>

#include <cstdint>

namespace lux::world::detail
{
    enum class EWorldFailurePoint : std::uint8_t
    {
        DESCRIPTION_MUTATION_ALLOCATION,
        DESCRIPTION_BUILD_ALLOCATION,
        DESCRIPTION_BUILD_SIZE_OVERFLOW,
        PARTITION_MUTATION_ALLOCATION,
        PARTITION_BUILD_ALLOCATION,
        PARTITION_BUILD_SIZE_OVERFLOW,
    };

    LUX_ENGINE_WORLD_PUBLIC void failNextWorldOperationForTest(
        EWorldFailurePoint point
    ) noexcept;

    [[nodiscard]] bool consumeWorldFailureForTest(
        EWorldFailurePoint point
    ) noexcept;
}
