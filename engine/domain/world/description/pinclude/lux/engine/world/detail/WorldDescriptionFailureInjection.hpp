#pragma once

#include <lux/engine/world/description/visibility.h>

#include <cstdint>

namespace lux::world::detail
{
    enum class EWorldDescriptionFailurePoint : std::uint8_t
    {
        MUTATION_ALLOCATION,
        BUILD_ALLOCATION,
        BUILD_SIZE_OVERFLOW,
    };

    LUX_ENGINE_WORLD_DESCRIPTION_PUBLIC void failNextWorldDescriptionOperationForTest(
        EWorldDescriptionFailurePoint point
    ) noexcept;

    [[nodiscard]] bool consumeWorldDescriptionFailureForTest(EWorldDescriptionFailurePoint point) noexcept;
} // namespace lux::world::detail
