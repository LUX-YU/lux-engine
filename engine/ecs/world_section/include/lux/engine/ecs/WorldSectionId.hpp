#pragma once

#include <uuid.h>

namespace lux::ecs
{
    struct WorldSectionId final
    {
        uuids::uuid value;

        [[nodiscard]] friend bool operator==(
            const WorldSectionId&,
            const WorldSectionId&
        ) noexcept = default;
    };
} // namespace lux::ecs
