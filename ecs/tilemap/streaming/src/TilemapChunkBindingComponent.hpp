#pragma once

#include <cstdint>

namespace lux::ecs::tilemap::streaming
{
    /// Private, non-reflected generation handle into TilemapChunkSystem.
    struct TilemapChunkBindingComponent final
    {
        std::uint32_t slot{~std::uint32_t{0u}};
        std::uint32_t generation{0u};
    };
} // namespace lux::ecs::tilemap::streaming
