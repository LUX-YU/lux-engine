#pragma once

#include <cstdint>
#include <type_traits>

namespace lux::rdesc
{
    enum class ETextureDimension : std::int32_t
    {
        TEX_2D = 0,
        TEX_2D_ARRAY = 1,
        TEX_3D = 2,
        CUBE = 3
    };

    enum class ETextureFormat : std::int32_t
    {
        UNDEFINED = 0,
        R8_UNORM = 1,
        R8_SNORM = 2,
        R8_UINT = 3,
        R8_SINT = 4,
        R8_SRGB = 5,
        R16_UNORM = 6,
        R16_SNORM = 7,
        R16_UINT = 8,
        R16_SINT = 9,
        R16_SFLOAT = 10,
        R32_UINT = 11,
        R32_SINT = 12,
        R32_SFLOAT = 13,
        RG8_UNORM = 14,
        RG8_SNORM = 15,
        RG8_UINT = 16,
        RG8_SINT = 17,
        RG8_SRGB = 18,
        RG16_UNORM = 19,
        RG16_SNORM = 20,
        RG16_UINT = 21,
        RG16_SINT = 22,
        RG16_SFLOAT = 23,
        RG32_UINT = 24,
        RG32_SINT = 25,
        RG32_SFLOAT = 26,
        RGB8_UNORM = 27,
        RGB8_SNORM = 28,
        RGB8_UINT = 29,
        RGB8_SINT = 30,
        RGB8_SRGB = 31,
        RGB32_UINT = 32,
        RGB32_SINT = 33,
        RGB32_SFLOAT = 34,
        RGBA8_UNORM = 35,
        RGBA8_SNORM = 36,
        RGBA8_UINT = 37,
        RGBA8_SINT = 38,
        RGBA8_SRGB = 39,
        RGBA16_UNORM = 40,
        RGBA16_SNORM = 41,
        RGBA16_UINT = 42,
        RGBA16_SINT = 43,
        RGBA16_SFLOAT = 44,
        RGBA32_UINT = 45,
        RGBA32_SINT = 46,
        RGBA32_SFLOAT = 47,
        BGRA8_UNORM = 48,
        BGRA8_SRGB = 49,
        D16_UNORM = 50,
        D32_SFLOAT = 51,
        D16_UNORM_S8_UINT = 52,
        D24_UNORM_S8_UINT = 53,
        D32_SFLOAT_S8_UINT = 54,
        BC1_RGB_UNORM = 55,
        BC1_RGB_SRGB = 56,
        BC1_RGBA_UNORM = 57,
        BC1_RGBA_SRGB = 58,
        BC2_UNORM = 59,
        BC2_SRGB = 60,
        BC3_UNORM = 61,
        BC3_SRGB = 62,
        BC4_UNORM = 63,
        BC4_SNORM = 64,
        BC5_UNORM = 65,
        BC5_SNORM = 66,
        BC6H_UFLOAT = 67,
        BC6H_SFLOAT = 68,
        BC7_UNORM = 69,
        BC7_SRGB = 70
    };

    static_assert(sizeof(ETextureDimension) == 4);
    static_assert(sizeof(ETextureFormat) == 4);
    static_assert(std::is_trivially_copyable_v<ETextureDimension>);
    static_assert(std::is_trivially_copyable_v<ETextureFormat>);
}
