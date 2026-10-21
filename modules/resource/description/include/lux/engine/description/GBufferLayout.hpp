#pragma once

#include <array>
#include <lux/engine/description/Image.hpp>
#include <string_view>

namespace lux::rdesc
{
    // World-space oct normal, forward [0,1] depth, linear colors, dense model tag (Unlit=0, PBR=1).
    // A different channel/space interpretation is a different semantic contract, even with identical formats.
    struct GBufferLayout final
    {
        static constexpr std::string_view semantic = "lux.surface.gbuffer.world-oct-forward-z.v1";
        static constexpr std::array color_formats{
            ETextureFormat::RGBA8_UNORM,
            ETextureFormat::RGBA16_SFLOAT,
            ETextureFormat::RGBA16_SFLOAT
        };
        static constexpr auto depth_format = ETextureFormat::D32_SFLOAT;
    };
} // namespace lux::rdesc
