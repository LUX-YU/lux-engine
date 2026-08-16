#pragma once

#include <cstdint>

namespace lux::runtime::spatial2d
{
    /// Private identity for a Pixel-domain prepared slot. Authored content
    /// never serializes or reflects this value.
    struct PixelChunkBindingComponent final
    {
        std::uint32_t slot{~std::uint32_t{0u}};
        std::uint32_t generation{0u};
    };
}
