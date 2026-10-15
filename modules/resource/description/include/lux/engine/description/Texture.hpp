#pragma once
#include <lux/engine/description/TextureInfo.hpp>
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
        [[nodiscard]] static lux::cxx::expected<Texture, ETextureCreateError> fromShared(
            TextureInfo info,
            lux::cxx::SharedBytes<> pixels
        ) noexcept;

        [[nodiscard]] static lux::cxx::expected<Texture, ETextureCreateError> copyOf(
            TextureInfo info,
            std::span<const std::byte> pixels
        );

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
        {}

        TextureInfo info_{};
        lux::cxx::SharedBytes<> pixels_;
    };
}
