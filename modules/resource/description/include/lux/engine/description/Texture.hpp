#pragma once
#include <lux/engine/resource/visibility.h>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <cstddef>
#include <cstdint>
#include <array>
#include <type_traits>
#include <utility>

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
        ASTC_4x4_UNORM,
        ASTC_4x4_SRGB,
        ASTC_6x6_UNORM,
        ASTC_6x6_SRGB,
    };

    enum class ETextureColorSpace : uint8_t
    {
        UNKNOWN = 0,
        SRGB,
        LINEAR,
        DATA,
    };

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
        case ETexturePixelFormat::ASTC_4x4_UNORM:
        case ETexturePixelFormat::ASTC_4x4_SRGB:
        case ETexturePixelFormat::ASTC_6x6_UNORM:
        case ETexturePixelFormat::ASTC_6x6_SRGB:
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

    /**
     * @brief Represents a loaded texture resource.
     *
     * The Texture class encapsulates texture data and provides access to
     * texture properties such as dimensions and pixel data. It manages
     * the lifetime of texture data and provides copy/move semantics.
     */
    class LUX_RESOURCE_PUBLIC Texture
    {
    public:
        /**
         * @brief Default constructor for Texture.
         */
        Texture();

        /**
         * @brief Constructs a Texture with the given information and data.
         * @param info Texture information structure
         * @param data Pointer to the texture pixel data
         * @param size Size of the texture data in bytes
         */
        [[nodiscard]] static lux::cxx::expected<Texture, ETextureCreateError>
        fromShared(TextureInfo info, lux::cxx::SharedBytes<> pixels) noexcept;

        [[nodiscard]] static lux::cxx::expected<Texture, ETextureCreateError>
        copyOf(TextureInfo info, std::span<const std::byte> pixels);

        Texture(const Texture&) = default;
        Texture& operator=(const Texture&) = default;
        Texture(Texture&&) noexcept = default;
        Texture& operator=(Texture&&) noexcept = default;
        ~Texture() = default;

        /**
         * @brief Gets the width of the texture.
         * @return Width in pixels
         */
        [[nodiscard]] int width() const
        {
            return info_.width;
        }

        /**
         * @brief Gets the height of the texture.
         * @return Height in pixels
         */
        [[nodiscard]] int height() const
        {
            return info_.height;
        }

        /**
         * @brief Gets the number of color channels.
         * @return Number of channels (e.g., 3 for RGB, 4 for RGBA)
         */
        [[nodiscard]] int channel() const
        {
            return info_.channel;
        }

        [[nodiscard]] ETexturePixelFormat pixelFormat() const
        {
            return info_.pixel_format;
        }

        [[nodiscard]] ETextureColorSpace colorSpace() const
        {
            return info_.color_space;
        }

        [[nodiscard]] uint32_t layers() const
        {
            return info_.layers;
        }

        [[nodiscard]] uint32_t mipCount() const
        {
            return info_.mip_count;
        }

        [[nodiscard]] uint32_t flags() const
        {
            return info_.flags;
        }

        [[nodiscard]] const TextureMipRange& mipRange(uint32_t level) const
        {
            if (level >= kTextureMaxMipCount)
                level = 0;
            return info_.mip_ranges[level];
        }

        [[nodiscard]] const TextureInfo& info() const
        {
            return info_;
        }

        /**
         * @brief Gets a const pointer to the texture data.
         * @return Const pointer to the pixel data
         */
        [[nodiscard]] const void* data() const
        {
            return pixels_.data();
        }

        [[nodiscard]] size_t size() const
        {
            return pixels_.size();
        }

        [[nodiscard]] const lux::cxx::SharedBytes<>& pixels() const noexcept
        {
            return pixels_;
        }

    private:
        Texture(TextureInfo info, lux::cxx::SharedBytes<> pixels) noexcept
            : info_(std::move(info)), pixels_(std::move(pixels))
        {
        }

        TextureInfo info_{};
        lux::cxx::SharedBytes<> pixels_;
    };
}
