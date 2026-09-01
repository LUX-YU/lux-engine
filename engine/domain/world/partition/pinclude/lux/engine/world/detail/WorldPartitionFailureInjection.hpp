#pragma once

#include <lux/engine/world/partition/visibility.h>

#include <cstdint>

namespace lux::world::detail
{
    enum class EWorldPartitionFailurePoint : std::uint8_t
    {
        MUTATION_ALLOCATION,
        BUILD_ALLOCATION,
        BUILD_SIZE_OVERFLOW,
    };

    LUX_ENGINE_WORLD_PARTITION_PUBLIC void failNextWorldPartitionOperationForTest(
        EWorldPartitionFailurePoint point
    ) noexcept;

    [[nodiscard]] bool consumeWorldPartitionFailureForTest(EWorldPartitionFailurePoint point) noexcept;
} // namespace lux::world::detail
