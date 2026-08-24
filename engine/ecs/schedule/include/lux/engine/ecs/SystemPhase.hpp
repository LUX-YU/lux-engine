#pragma once

#include <cstdint>

namespace lux::ecs
{
    enum class SystemPhase : std::uint8_t
    {
        PreUpdate,
        Update,
        PostUpdate,
    };
} // namespace lux::ecs
