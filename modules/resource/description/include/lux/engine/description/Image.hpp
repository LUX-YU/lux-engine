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

    enum class ETextureFormatClass
    {
        INVALID,
        COLOR,
        DEPTH,
        DEPTH_STENCIL
    };
    enum class ETextureUsage
    {
        SAMPLED,
        STORAGE,
        COLOR_ATTACHMENT,
        DEPTH_STENCIL_ATTACHMENT,
        TRANSFER
    };

    [[nodiscard]] constexpr ETextureFormatClass textureFormatClass(ETextureFormat format) noexcept
    {
        switch (format)
        {
        case ETextureFormat::R8_UNORM:
        case ETextureFormat::R8_SNORM:
        case ETextureFormat::R8_UINT:
        case ETextureFormat::R8_SINT:
        case ETextureFormat::R8_SRGB:
        case ETextureFormat::R16_UNORM:
        case ETextureFormat::R16_SNORM:
        case ETextureFormat::R16_UINT:
        case ETextureFormat::R16_SINT:
        case ETextureFormat::R16_SFLOAT:
        case ETextureFormat::R32_UINT:
        case ETextureFormat::R32_SINT:
        case ETextureFormat::R32_SFLOAT:
        case ETextureFormat::RG8_UNORM:
        case ETextureFormat::RG8_SNORM:
        case ETextureFormat::RG8_UINT:
        case ETextureFormat::RG8_SINT:
        case ETextureFormat::RG8_SRGB:
        case ETextureFormat::RG16_UNORM:
        case ETextureFormat::RG16_SNORM:
        case ETextureFormat::RG16_UINT:
        case ETextureFormat::RG16_SINT:
        case ETextureFormat::RG16_SFLOAT:
        case ETextureFormat::RG32_UINT:
        case ETextureFormat::RG32_SINT:
        case ETextureFormat::RG32_SFLOAT:
        case ETextureFormat::RGB8_UNORM:
        case ETextureFormat::RGB8_SNORM:
        case ETextureFormat::RGB8_UINT:
        case ETextureFormat::RGB8_SINT:
        case ETextureFormat::RGB8_SRGB:
        case ETextureFormat::RGB32_UINT:
        case ETextureFormat::RGB32_SINT:
        case ETextureFormat::RGB32_SFLOAT:
        case ETextureFormat::RGBA8_UNORM:
        case ETextureFormat::RGBA8_SNORM:
        case ETextureFormat::RGBA8_UINT:
        case ETextureFormat::RGBA8_SINT:
        case ETextureFormat::RGBA8_SRGB:
        case ETextureFormat::RGBA16_UNORM:
        case ETextureFormat::RGBA16_SNORM:
        case ETextureFormat::RGBA16_UINT:
        case ETextureFormat::RGBA16_SINT:
        case ETextureFormat::RGBA16_SFLOAT:
        case ETextureFormat::RGBA32_UINT:
        case ETextureFormat::RGBA32_SINT:
        case ETextureFormat::RGBA32_SFLOAT:
        case ETextureFormat::BGRA8_UNORM:
        case ETextureFormat::BGRA8_SRGB:
        case ETextureFormat::BC1_RGB_UNORM:
        case ETextureFormat::BC1_RGB_SRGB:
        case ETextureFormat::BC1_RGBA_UNORM:
        case ETextureFormat::BC1_RGBA_SRGB:
        case ETextureFormat::BC2_UNORM:
        case ETextureFormat::BC2_SRGB:
        case ETextureFormat::BC3_UNORM:
        case ETextureFormat::BC3_SRGB:
        case ETextureFormat::BC4_UNORM:
        case ETextureFormat::BC4_SNORM:
        case ETextureFormat::BC5_UNORM:
        case ETextureFormat::BC5_SNORM:
        case ETextureFormat::BC6H_UFLOAT:
        case ETextureFormat::BC6H_SFLOAT:
        case ETextureFormat::BC7_UNORM:
        case ETextureFormat::BC7_SRGB:
            return ETextureFormatClass::COLOR;
        case ETextureFormat::D16_UNORM:
        case ETextureFormat::D32_SFLOAT:
            return ETextureFormatClass::DEPTH;
        case ETextureFormat::D16_UNORM_S8_UINT:
        case ETextureFormat::D24_UNORM_S8_UINT:
        case ETextureFormat::D32_SFLOAT_S8_UINT:
            return ETextureFormatClass::DEPTH_STENCIL;
        default:
            return ETextureFormatClass::INVALID;
        }
    }

    [[nodiscard]] constexpr bool isSrgbTextureFormat(ETextureFormat format) noexcept
    {
        switch (format)
        {
        case ETextureFormat::R8_SRGB:
        case ETextureFormat::RG8_SRGB:
        case ETextureFormat::RGB8_SRGB:
        case ETextureFormat::RGBA8_SRGB:
        case ETextureFormat::BGRA8_SRGB:
        case ETextureFormat::BC1_RGB_SRGB:
        case ETextureFormat::BC1_RGBA_SRGB:
        case ETextureFormat::BC2_SRGB:
        case ETextureFormat::BC3_SRGB:
        case ETextureFormat::BC7_SRGB:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] constexpr bool isCompressedTextureFormat(ETextureFormat format) noexcept
    {
        switch (format)
        {
        case ETextureFormat::BC1_RGB_UNORM:
        case ETextureFormat::BC1_RGB_SRGB:
        case ETextureFormat::BC1_RGBA_UNORM:
        case ETextureFormat::BC1_RGBA_SRGB:
        case ETextureFormat::BC2_UNORM:
        case ETextureFormat::BC2_SRGB:
        case ETextureFormat::BC3_UNORM:
        case ETextureFormat::BC3_SRGB:
        case ETextureFormat::BC4_UNORM:
        case ETextureFormat::BC4_SNORM:
        case ETextureFormat::BC5_UNORM:
        case ETextureFormat::BC5_SNORM:
        case ETextureFormat::BC6H_UFLOAT:
        case ETextureFormat::BC6H_SFLOAT:
        case ETextureFormat::BC7_UNORM:
        case ETextureFormat::BC7_SRGB:
            return true;
        default:
            return false;
        }
    }

    // Logical format/usage eligibility, not a promise about a physical device's format features.
    [[nodiscard]] constexpr bool supportsTextureUsage(ETextureFormat format, ETextureUsage usage) noexcept
    {
        const auto kind = textureFormatClass(format);
        if (kind == ETextureFormatClass::INVALID)
        {
            return false;
        }
        switch (usage)
        {
        case ETextureUsage::SAMPLED:
        case ETextureUsage::TRANSFER:
            return true;
        case ETextureUsage::DEPTH_STENCIL_ATTACHMENT:
            return kind == ETextureFormatClass::DEPTH || kind == ETextureFormatClass::DEPTH_STENCIL;
        case ETextureUsage::COLOR_ATTACHMENT:
            return kind == ETextureFormatClass::COLOR && !isCompressedTextureFormat(format);
        case ETextureUsage::STORAGE:
            switch (format)
            {
            case ETextureFormat::R8_UNORM:
            case ETextureFormat::R8_SNORM:
            case ETextureFormat::R8_UINT:
            case ETextureFormat::R8_SINT:
            case ETextureFormat::R16_UNORM:
            case ETextureFormat::R16_SNORM:
            case ETextureFormat::R16_UINT:
            case ETextureFormat::R16_SINT:
            case ETextureFormat::R16_SFLOAT:
            case ETextureFormat::R32_UINT:
            case ETextureFormat::R32_SINT:
            case ETextureFormat::R32_SFLOAT:
            case ETextureFormat::RG8_UNORM:
            case ETextureFormat::RG8_SNORM:
            case ETextureFormat::RG8_UINT:
            case ETextureFormat::RG8_SINT:
            case ETextureFormat::RG16_UNORM:
            case ETextureFormat::RG16_SNORM:
            case ETextureFormat::RG16_UINT:
            case ETextureFormat::RG16_SINT:
            case ETextureFormat::RG16_SFLOAT:
            case ETextureFormat::RG32_UINT:
            case ETextureFormat::RG32_SINT:
            case ETextureFormat::RG32_SFLOAT:
            case ETextureFormat::RGBA8_UNORM:
            case ETextureFormat::RGBA8_SNORM:
            case ETextureFormat::RGBA8_UINT:
            case ETextureFormat::RGBA8_SINT:
            case ETextureFormat::RGBA16_UNORM:
            case ETextureFormat::RGBA16_SNORM:
            case ETextureFormat::RGBA16_UINT:
            case ETextureFormat::RGBA16_SINT:
            case ETextureFormat::RGBA16_SFLOAT:
            case ETextureFormat::RGBA32_UINT:
            case ETextureFormat::RGBA32_SINT:
            case ETextureFormat::RGBA32_SFLOAT:
                return true;
            default:
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] constexpr std::uint32_t textureAspectMask(ETextureFormat format) noexcept
    {
        switch (textureFormatClass(format))
        {
        case ETextureFormatClass::COLOR:
            return 1;
        case ETextureFormatClass::DEPTH:
            return 2;
        case ETextureFormatClass::DEPTH_STENCIL:
            return 6;
        default:
            return 0;
        }
    }

    static_assert(sizeof(ETextureDimension) == 4);
    static_assert(sizeof(ETextureFormat) == 4);
    static_assert(std::is_trivially_copyable_v<ETextureDimension>);
    static_assert(std::is_trivially_copyable_v<ETextureFormat>);
} // namespace lux::rdesc
