#pragma once

#include <array>
#include <cstdint>
#include <lux/engine/description/Image.hpp>
#include <type_traits>

namespace lux::rdesc
{
    inline constexpr uint32_t kTextureMaxMipCount = 16;

    enum class ETexturePixelFormat : uint32_t
    {
        UNKNOWN = 0,
        RGBA8_UNORM,
        RGBA8_SRGB,
        RG8_UNORM,
        R8_UNORM,
        RGBA16_SFLOAT,
        BC1_SRGB,
        BC3_SRGB,
        BC5_UNORM,
        BC7_SRGB,
        ETC2_RGB8_UNORM,
        ETC2_RGB8_SRGB,
        ETC2_RGBA8_UNORM,
        ETC2_RGBA8_SRGB,
        ASTC_4X4_UNORM,
        ASTC_4X4_SRGB,
        ASTC_6X6_UNORM,
        ASTC_6X6_SRGB,
    };

    enum class ETextureColorSpace : uint8_t
    {
        UNKNOWN = 0,
        SRGB,
        LINEAR,
        DATA,
    };

    // Projects an encoded texture's pixel representation into the common image contract.
    // Unsupported representations remain explicit; no implicit RGBA conversion is performed.
    [[nodiscard]] constexpr ETextureFormat textureImageFormat(ETexturePixelFormat format) noexcept
    {
        switch (format)
        {
        case ETexturePixelFormat::RGBA8_UNORM:
            return ETextureFormat::RGBA8_UNORM;
        case ETexturePixelFormat::RGBA8_SRGB:
            return ETextureFormat::RGBA8_SRGB;
        case ETexturePixelFormat::RG8_UNORM:
            return ETextureFormat::RG8_UNORM;
        case ETexturePixelFormat::R8_UNORM:
            return ETextureFormat::R8_UNORM;
        case ETexturePixelFormat::RGBA16_SFLOAT:
            return ETextureFormat::RGBA16_SFLOAT;
        case ETexturePixelFormat::BC1_SRGB:
            return ETextureFormat::BC1_RGBA_SRGB;
        case ETexturePixelFormat::BC3_SRGB:
            return ETextureFormat::BC3_SRGB;
        case ETexturePixelFormat::BC5_UNORM:
            return ETextureFormat::BC5_UNORM;
        case ETexturePixelFormat::BC7_SRGB:
            return ETextureFormat::BC7_SRGB;
        default:
            return ETextureFormat::UNDEFINED;
        }
    }

    enum class ETextureAssetFlags : uint32_t
    {
        NONE = 0u,
        COMPRESSED = 1u << 0,
        PREMULTIPLIED_ALPHA = 1u << 1,
        /// The AUTHOR intends this texture to have NO mip chain: tileset
        /// atlases, pixel-art image sheets, index/lookup textures — anything
        /// whose minified average is meaningless or actively wrong.
        ///
        /// Why a flag and not `mip_count == 1`: mip_count describes the DATA
        /// the asset carries, and virtually every imported texture ships mip 0
        /// only, letting the GPU generate the chain. Reading intent off
        /// mip_count would silently strip mips from all 3D material textures.
        /// Absent flag = "generate a chain" (the historical behaviour).
        ///
        /// Recorded 2026-07-10: unconditional mip generation is what turned a
        /// tile-shader LOD mistake into whole-atlas colour bleed at every tile
        /// seam (the seam pixels sampled the smallest mip = the atlas average).
        NO_MIPS = 1u << 2,
    };

    [[nodiscard]] inline constexpr uint32_t toUnderlying(ETextureAssetFlags v) noexcept
    {
        return static_cast<uint32_t>(v);
    }

    [[nodiscard]] inline constexpr bool hasTextureFlag(uint32_t flags, ETextureAssetFlags bit) noexcept
    {
        return (flags & toUnderlying(bit)) != 0u;
    }

    [[nodiscard]] inline constexpr bool isCompressedFormat(ETexturePixelFormat fmt) noexcept
    {
        switch (fmt)
        {
        case ETexturePixelFormat::BC1_SRGB:
        case ETexturePixelFormat::BC3_SRGB:
        case ETexturePixelFormat::BC5_UNORM:
        case ETexturePixelFormat::BC7_SRGB:
        case ETexturePixelFormat::ETC2_RGB8_UNORM:
        case ETexturePixelFormat::ETC2_RGB8_SRGB:
        case ETexturePixelFormat::ETC2_RGBA8_UNORM:
        case ETexturePixelFormat::ETC2_RGBA8_SRGB:
        case ETexturePixelFormat::ASTC_4X4_UNORM:
        case ETexturePixelFormat::ASTC_4X4_SRGB:
        case ETexturePixelFormat::ASTC_6X6_UNORM:
        case ETexturePixelFormat::ASTC_6X6_SRGB:
            return true;
        default:
            return false;
        }
    }

    struct TextureMipRange
    {
        uint64_t offset{0};
        uint64_t size{0};
        uint32_t width{0};
        uint32_t height{0};
    };

    /**
     * @brief Structure containing basic texture information.
     *
     * This structure holds the fundamental properties of a texture,
     * including its dimensions and color channel information.
     */
    struct TextureInfo
    {
        int width;   ///< Width of the texture in pixels
        int height;  ///< Height of the texture in pixels
        int channel; ///< Number of color channels (e.g., 3 for RGB, 4 for RGBA)
        ETexturePixelFormat pixel_format{ETexturePixelFormat::RGBA8_SRGB};
        ETextureColorSpace color_space{ETextureColorSpace::SRGB};
        uint32_t layers{1};
        uint32_t mip_count{1};
        uint32_t flags{toUnderlying(ETextureAssetFlags::NONE)};
        std::array<TextureMipRange, kTextureMaxMipCount> mip_ranges{};
    };

    enum class ETextureCreateError : uint8_t
    {
        PIXEL_DATA_REQUIRED,
        DIMENSIONS_INVALID
    };

    // for serialization/deserialization (POD – written directly into .luxasset)
    struct TextureAssetInfo
    {
        int32_t width{0};   ///< Width of the texture in pixels
        int32_t height{0};  ///< Height of the texture in pixels
        int32_t channel{0}; ///< Number of colour channels (e.g. 4 for RGBA)
        uint32_t layers{1};
        uint32_t mip_count{1};
        uint32_t pixel_format{static_cast<uint32_t>(ETexturePixelFormat::RGBA8_SRGB)};
        uint32_t color_space{static_cast<uint32_t>(ETextureColorSpace::SRGB)};
        uint32_t flags{toUnderlying(ETextureAssetFlags::NONE)};
        std::array<TextureMipRange, kTextureMaxMipCount> mip_ranges{};
    };

    static_assert(std::is_standard_layout_v<TextureAssetInfo>, "TextureAssetInfo must be standard layout");

} // namespace lux::rdesc
