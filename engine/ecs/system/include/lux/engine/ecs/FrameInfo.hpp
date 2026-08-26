#pragma once

#include <cstdint>

namespace lux::ecs
{
    struct FrameInfo final
    {
        float delta_seconds{};
        std::uint64_t tick_index{};
    };
}
