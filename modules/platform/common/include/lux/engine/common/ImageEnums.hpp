#pragma once
/**
 * @file ImageEnums.hpp
 * @brief Platform-agnostic image / texture enumerations.
 *
 * These enums describe texture dimensionality, usage role, and pixel
 * format at the RHI-abstraction level.  They are intentionally free
 * of any graphics-API header dependency (no Vulkan, no Metal, no D3D).
 *
 * Conversion to native API types is done in the respective backend
 * modules (e.g. render/utils/vk_convert.hpp, render/graph/vk_type_converter.hpp).
 */

namespace lux::common
{
    // ========== Texture dimension ==========

    enum class ETextureDimension
    {
        TEX_2D,
        TEX_2D_ARRAY,
        TEX_3D,
        CUBE
    };

    // ========== Texture role ==========

    enum class ETextureRole
    {
        COLOR_ATTACHMENT,
        DEPTH_STENCIL_ATTACHMENT,
        SAMPLED,
        UNORDERED_ACCESS,
        INPUT_ATTACHMENT
    };

    // ========== Texture format ==========

    enum class ETextureFormat
    {
        UNDEFINED,
        // 8-bit formats
        R8_UNORM, R8_SNORM, R8_UINT, R8_SINT, R8_SRGB,
        // 16-bit formats
        R16_UNORM, R16_SNORM, R16_UINT, R16_SINT, R16_SFLOAT,
        // 32-bit formats
        R32_UINT, R32_SINT, R32_SFLOAT,
        // RG 8-bit formats
        RG8_UNORM, RG8_SNORM, RG8_UINT, RG8_SINT, RG8_SRGB,
        // RG 16-bit formats
        RG16_UNORM, RG16_SNORM, RG16_UINT, RG16_SINT, RG16_SFLOAT,
        // RG 32-bit formats
        RG32_UINT, RG32_SINT, RG32_SFLOAT,
        // RGB 8-bit formats
        RGB8_UNORM, RGB8_SNORM, RGB8_UINT, RGB8_SINT, RGB8_SRGB,
        // RGB 32-bit formats
        RGB32_UINT, RGB32_SINT, RGB32_SFLOAT,
        // RGBA 8-bit formats
        RGBA8_UNORM, RGBA8_SNORM, RGBA8_UINT, RGBA8_SINT, RGBA8_SRGB,
        // RGBA 16-bit formats
        RGBA16_UNORM, RGBA16_SNORM, RGBA16_UINT, RGBA16_SINT, RGBA16_SFLOAT,
        // RGBA 32-bit formats
        RGBA32_UINT, RGBA32_SINT, RGBA32_SFLOAT,
        // BGRA formats
        BGRA8_UNORM, BGRA8_SRGB,
        // Depth formats
        D16_UNORM, D32_SFLOAT,
        // Depth-stencil formats
        D16_UNORM_S8_UINT, D24_UNORM_S8_UINT, D32_SFLOAT_S8_UINT,
        // BC compressed formats
        BC1_RGB_UNORM, BC1_RGB_SRGB, BC1_RGBA_UNORM, BC1_RGBA_SRGB,
        BC2_UNORM, BC2_SRGB,
        BC3_UNORM, BC3_SRGB,
        BC4_UNORM, BC4_SNORM,
        BC5_UNORM, BC5_SNORM,
        BC6H_UFLOAT, BC6H_SFLOAT,
        BC7_UNORM, BC7_SRGB
    };
} // namespace lux::common
