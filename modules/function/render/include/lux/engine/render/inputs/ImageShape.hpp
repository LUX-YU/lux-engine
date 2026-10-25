#pragma once

#include <cstdint>
#include <lux/engine/description/Image.hpp>

namespace lux::render
{
    // CPU shape of a supplied sampled image; no Graph allocation, origin, access or binding policy.
    struct ImageShape final
    {
        std::uint32_t width = 1, height = 1, layers = 1, mips = 1;
        rdesc::ETextureFormat format = rdesc::ETextureFormat::RGBA8_UNORM;
        bool operator==(const ImageShape&) const noexcept = default;
    };
} // namespace lux::render
