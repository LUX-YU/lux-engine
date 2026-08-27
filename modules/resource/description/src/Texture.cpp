#include <lux/engine/description/Texture.hpp>

namespace lux::rdesc
{
    Texture::Texture() = default;

    lux::cxx::expected<Texture, ETextureCreateError>
    Texture::fromShared(TextureInfo info, lux::cxx::SharedBytes<> pixels) noexcept
    {
        if (info.width <= 0 || info.height <= 0 || info.channel <= 0)
            return lux::cxx::unexpected(ETextureCreateError::DIMENSIONS_INVALID);
        if (pixels.empty())
            return lux::cxx::unexpected(ETextureCreateError::PIXEL_DATA_REQUIRED);
        return Texture{std::move(info), std::move(pixels)};
    }

    lux::cxx::expected<Texture, ETextureCreateError>
    Texture::copyOf(TextureInfo info, std::span<const std::byte> pixels)
    {
        return fromShared(std::move(info), lux::cxx::SharedBytes<>::copyOf(pixels));
    }
}
