#pragma once

#include <lux/engine/ecs/SystemSetId.hpp>

#include <cstdint>

namespace lux::ecs
{
    enum class ESystemOrder : std::uint8_t
    {
        BEFORE,
        AFTER,
    };

    struct SystemOrder final
    {
        ESystemOrder relation{ESystemOrder::AFTER};
        SystemSetId target{};
        bool required{};
    };
} // namespace lux::ecs
