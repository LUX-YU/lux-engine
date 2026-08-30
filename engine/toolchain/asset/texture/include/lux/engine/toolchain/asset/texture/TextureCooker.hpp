#pragma once

#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/toolchain/asset/texture/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::toolchain
{
    enum class ETextureCookError : std::uint8_t
    {
        INVALID_SOURCE,
        INVALID_OPTIONS,
        UNSUPPORTED_FORMAT,
        RANGE_OVERFLOW,
        DECODE_FAILED,
        ALLOCATION_FAILURE,
        INVALID_COOKED_TEXTURE,
    };

    struct TextureCookFailure final
    {
        ETextureCookError code{ETextureCookError::INVALID_SOURCE};
        std::size_t offset{};
    };

    struct TextureCookConfiguration final
    {
        lux::rdesc::ETexturePixelFormat output_format{lux::rdesc::ETexturePixelFormat::BC7_SRGB};
        lux::rdesc::ETextureColorSpace color_space{lux::rdesc::ETextureColorSpace::SRGB};
        bool no_mips{};
    };

    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_TEXTURE_PUBLIC lux::cxx::expected<
        std::shared_ptr<const lux::asset::TextureAsset>,
        TextureCookFailure
    > cookTexture(
        lux::asset::AssetInfo metadata,
        lux::cxx::SharedBytes<> authoring_image,
        const TextureCookConfiguration& configuration
    ) noexcept;
} // namespace lux::toolchain
