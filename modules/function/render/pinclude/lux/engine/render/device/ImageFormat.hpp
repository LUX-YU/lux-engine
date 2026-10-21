#pragma once

#include <NRI.h>
#include <lux/engine/description/Image.hpp>

namespace lux::render::detail
{
    struct ImageFormat final
    {
        nri::Format native = nri::Format::UNKNOWN;
        std::uint32_t bytes = 0;
        nri::PlaneBits aspects = nri::PlaneBits::COLOR;
        std::uint32_t block = 1;
    };

    [[nodiscard]] constexpr ImageFormat imageFormat(rdesc::ETextureFormat value) noexcept
    {
        using F = rdesc::ETextureFormat;
        const auto bytes = rdesc::textureBlockBytes(value);
        switch (value)
        {
        case F::RGBA8_UNORM:
            return {nri::Format::RGBA8_UNORM, bytes};
        case F::RGBA8_SRGB:
            return {nri::Format::RGBA8_SRGB, bytes};
        case F::BGRA8_UNORM:
            return {nri::Format::BGRA8_UNORM, bytes};
        case F::BGRA8_SRGB:
            return {nri::Format::BGRA8_SRGB, bytes};
        case F::RGBA16_SFLOAT:
            return {nri::Format::RGBA16_SFLOAT, bytes};
        case F::RGBA32_SFLOAT:
            return {nri::Format::RGBA32_SFLOAT, bytes};
        case F::D32_SFLOAT:
            return {nri::Format::D32_SFLOAT, bytes, nri::PlaneBits::DEPTH};
        case F::D16_UNORM:
            return {nri::Format::D16_UNORM, bytes, nri::PlaneBits::DEPTH};
        case F::BC1_RGBA_SRGB:
            return {nri::Format::BC1_RGBA_SRGB, bytes, nri::PlaneBits::COLOR, 4};
        default:
            return {};
        }
    }
} // namespace lux::render::detail
