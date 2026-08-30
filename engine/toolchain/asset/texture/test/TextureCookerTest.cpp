#include <lux/cxx/algorithm/sha256.hpp>

#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/toolchain/asset/texture/TextureCooker.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
    void write16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value)
    {
        bytes[offset] = static_cast<std::byte>(value & 0xFFU);
        bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    }

    void write32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
    {
        for (std::size_t index = 0U; index < 4U; ++index)
            bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }

    [[nodiscard]] std::vector<std::byte> makeBmp(std::uint32_t width, std::uint32_t height)
    {
        const std::size_t pixel_bytes = static_cast<std::size_t>(width) * height * 4U;
        std::vector<std::byte> result(54U + pixel_bytes);
        result[0] = std::byte{'B'};
        result[1] = std::byte{'M'};
        write32(result, 2U, static_cast<std::uint32_t>(result.size()));
        write32(result, 10U, 54U);
        write32(result, 14U, 40U);
        write32(result, 18U, width);
        write32(result, 22U, height);
        write16(result, 26U, 1U);
        write16(result, 28U, 32U);
        write32(result, 34U, static_cast<std::uint32_t>(pixel_bytes));
        for (std::uint32_t y = 0U; y < height; ++y)
        {
            for (std::uint32_t x = 0U; x < width; ++x)
            {
                const std::size_t offset = 54U + (static_cast<std::size_t>(y) * width + x) * 4U;
                result[offset + 0U] = static_cast<std::byte>((x * 19U + y * 3U) & 0xFFU);
                result[offset + 1U] = static_cast<std::byte>((x * 7U + y * 31U) & 0xFFU);
                result[offset + 2U] = static_cast<std::byte>((x * 13U + y * 11U) & 0xFFU);
                result[offset + 3U] = std::byte{0xFFU};
            }
        }
        return result;
    }

    [[nodiscard]] lux::asset::AssetInfo metadata(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = tail;
        lux::asset::AssetInfo result{};
        result.id = lux::asset::AssetId{bytes};
        result.type = lux::asset::TextureAsset::asset_type;
        return result;
    }

    [[nodiscard]] std::shared_ptr<const lux::asset::TextureAsset> cook(
        std::span<const std::byte> source,
        lux::rdesc::ETexturePixelFormat format,
        lux::rdesc::ETextureColorSpace color_space,
        bool no_mips,
        std::uint8_t id
    )
    {
        const auto cooked = lux::toolchain::cookTexture(
            metadata(id),
            lux::cxx::SharedBytes<>::copyOf(source),
            lux::toolchain::TextureCookConfiguration{format, color_space, no_mips}
        );
        assert(cooked);
        return *cooked;
    }
} // namespace

int main()
{
    using lux::rdesc::ETextureAssetFlags;
    using lux::rdesc::ETextureColorSpace;
    using lux::rdesc::ETexturePixelFormat;

    const auto odd_image = makeBmp(5U, 3U);
    constexpr std::array raw_formats{
        ETexturePixelFormat::RGBA8_UNORM,
        ETexturePixelFormat::RGBA8_SRGB,
        ETexturePixelFormat::RG8_UNORM,
        ETexturePixelFormat::R8_UNORM,
    };
    std::uint8_t id = 1U;
    for (const auto format : raw_formats)
    {
        const auto asset = cook(odd_image, format, ETextureColorSpace::LINEAR, false, id++);
        assert(asset->data().width() == 5);
        assert(asset->data().height() == 3);
        assert(asset->data().mipCount() == 3U);
        assert(asset->data().mipRange(0U).width == 5U);
        assert(asset->data().mipRange(1U).width == 2U);
        assert(asset->data().mipRange(2U).width == 1U);
    }

    constexpr std::array compressed_formats{
        ETexturePixelFormat::BC1_SRGB,
        ETexturePixelFormat::BC3_SRGB,
        ETexturePixelFormat::BC5_UNORM,
        ETexturePixelFormat::BC7_SRGB,
    };
    for (const auto format : compressed_formats)
    {
        const auto color_space = format == ETexturePixelFormat::BC5_UNORM
            ? ETextureColorSpace::DATA
            : ETextureColorSpace::SRGB;
        const auto asset = cook(odd_image, format, color_space, false, id++);
        assert(asset->data().mipCount() == 3U);
        assert(lux::rdesc::hasTextureFlag(asset->data().flags(), ETextureAssetFlags::COMPRESSED));
        assert(asset->data().mipRange(0U).size ==
            (format == ETexturePixelFormat::BC1_SRGB ? 16U : 32U));
    }

    const auto no_mips = cook(
        odd_image,
        ETexturePixelFormat::BC3_SRGB,
        ETextureColorSpace::SRGB,
        true,
        id++
    );
    assert(no_mips->data().mipCount() == 1U);
    assert(lux::rdesc::hasTextureFlag(no_mips->data().flags(), ETextureAssetFlags::NO_MIPS));

    const auto narrow_image = makeBmp(1U, 7U);
    const auto narrow = cook(
        narrow_image,
        ETexturePixelFormat::BC7_SRGB,
        ETextureColorSpace::SRGB,
        false,
        id++
    );
    assert(narrow->data().mipCount() == 3U);
    assert(narrow->data().mipRange(1U).height == 3U);
    assert(narrow->data().mipRange(2U).height == 1U);

    constexpr lux::asset::AssetEncodeLimits encode_limits{1024U * 1024U};
    const auto first = lux::asset::TAssetSerDeser<lux::asset::TextureAsset>::encode(*narrow, encode_limits);
    const auto second = lux::asset::TAssetSerDeser<lux::asset::TextureAsset>::encode(*narrow, encode_limits);
    assert(first && second && *first == *second);
    assert(lux::cxx::algorithm::Sha256::hash(*first) == lux::cxx::algorithm::Sha256::hash(*second));

    const auto unsupported = lux::toolchain::cookTexture(
        metadata(id++),
        lux::cxx::SharedBytes<>::copyOf(odd_image),
        lux::toolchain::TextureCookConfiguration{
            ETexturePixelFormat::ASTC_4x4_SRGB,
            ETextureColorSpace::SRGB,
            false
        }
    );
    assert(!unsupported && unsupported.error().code == lux::toolchain::ETextureCookError::UNSUPPORTED_FORMAT);

    constexpr std::array corrupt{std::byte{0x01U}, std::byte{0x02U}, std::byte{0x03U}};
    const auto corrupt_result = lux::toolchain::cookTexture(
        metadata(id),
        lux::cxx::SharedBytes<>::copyOf(corrupt),
        lux::toolchain::TextureCookConfiguration{}
    );
    assert(!corrupt_result && corrupt_result.error().code == lux::toolchain::ETextureCookError::DECODE_FAILED);
    return 0;
}
