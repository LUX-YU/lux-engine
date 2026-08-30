#include <lux/cxx/algorithm/sha256.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace
{
    [[nodiscard]] lux::asset::AssetId fixtureId()
    {
        std::array<std::uint8_t, 16U> bytes{
            0x31U, 0x90U, 0x14U, 0x00U,
            0x00U, 0x00U, 0x40U, 0x00U,
            0x80U, 0x00U, 0x00U, 0x00U,
            0x00U, 0x00U, 0x00U, 0x01U,
        };
        return lux::asset::AssetId{bytes};
    }

    [[nodiscard]] std::string sha256(std::span<const std::byte> bytes)
    {
        const auto digest = lux::cxx::algorithm::Sha256::hash(bytes);
        std::array<char, lux::cxx::algorithm::Sha256Digest::hex_size> result{};
        digest.formatHex(result);
        return {result.data(), result.size()};
    }

    [[nodiscard]] lux::asset::AssetInfo fixtureInfo()
    {
        lux::asset::AssetInfo info{};
        info.id = fixtureId();
        info.type = lux::asset::TextureAsset::asset_type;
        info.date = 0x0102030405060708ULL;
        constexpr std::string_view display{"wire-contract"};
        std::memcpy(info.display_name.data(), display.data(), display.size());
        return info;
    }

    [[nodiscard]] std::shared_ptr<const lux::rdesc::Texture> fixtureTexture()
    {
        lux::rdesc::TextureInfo info{};
        info.width = 1;
        info.height = 1;
        info.channel = 4;
        info.mip_ranges[0] = {0U, 4U, 1U, 1U};
        constexpr std::array pixels{
            std::byte{0x11U}, std::byte{0x22U}, std::byte{0x33U}, std::byte{0xFFU}
        };
        auto texture = lux::rdesc::Texture::copyOf(info, pixels);
        assert(texture);
        return std::make_shared<const lux::rdesc::Texture>(std::move(*texture));
    }
} // namespace

int main()
{
    using namespace lux::asset;
    constexpr AssetDecodeLimits decode_limits{1024U * 1024U, 1024U * 1024U, 8U};
    constexpr AssetEncodeLimits encode_limits{1024U * 1024U};

    auto asset = TextureAsset::create(fixtureInfo(), fixtureTexture());
    assert(asset);
    auto encoded = TAssetSerDeser<TextureAsset>::encode(**asset, encode_limits);
    assert(encoded);
    assert(encoded->size() == 820U);
    assert(sha256(*encoded) == "e01de6ccfb600f997b0ad08035acbda1c404647faa86284e4dcd28a03efed3cc");

    auto owner = lux::cxx::SharedBytes<>::copyOf(*encoded);
    const std::byte* owned_pixels{};
    {
        const auto inspected = inspectCookedAssetImage(fixtureId(), owner, decode_limits);
        assert(inspected);
        owned_pixels = inspected->data().data();
    }
    assert(owned_pixels != nullptr);

    auto decoded = TAssetSerDeser<TextureAsset>::decode(fixtureId(), owner, decode_limits);
    assert(decoded);
    assert((*decoded)->id() == fixtureId());
    assert((*decoded)->type() == TextureAsset::asset_type);
    assert((*decoded)->as<TextureAsset>() == decoded->get());
    assert((*decoded)->data().pixels().data() == owned_pixels);
    assert((*decoded)->data().pixels().use_count() > 1L);

    owner = {};
    assert((*decoded)->data().size() == 4U);
    assert(static_cast<const std::byte*>((*decoded)->data().data())[0] == std::byte{0x11U});
    auto reencoded = TAssetSerDeser<TextureAsset>::encode(**decoded, encode_limits);
    assert(reencoded && *reencoded == *encoded);

    auto bad_magic = *encoded;
    bad_magic[0] ^= std::byte{0xFFU};
    const auto bad_magic_result = TAssetSerDeser<TextureAsset>::decode(
        fixtureId(),
        lux::cxx::SharedBytes<>::copyOf(bad_magic),
        decode_limits
    );
    assert(!bad_magic_result && bad_magic_result.error().code == EAssetDecodeError::INVALID_MAGIC);

    auto bad_version = *encoded;
    constexpr std::uint32_t bad_version_value = 0xFFFFFFFFU;
    std::memcpy(bad_version.data() + sizeof(std::uint32_t), &bad_version_value, sizeof(bad_version_value));
    const auto bad_version_result = TAssetSerDeser<TextureAsset>::decode(
        fixtureId(),
        lux::cxx::SharedBytes<>::copyOf(bad_version),
        decode_limits
    );
    assert(!bad_version_result && bad_version_result.error().code == EAssetDecodeError::UNSUPPORTED_VERSION);

    auto truncated = *encoded;
    truncated.pop_back();
    assert(!TAssetSerDeser<TextureAsset>::decode(
        fixtureId(),
        lux::cxx::SharedBytes<>::copyOf(truncated),
        decode_limits
    ));

    auto wrong_range = *encoded;
    lux::rdesc::TextureAssetInfo disk{};
    std::memcpy(&disk, wrong_range.data() + 400U, sizeof(disk));
    disk.mip_ranges[0].size = 3U;
    std::memcpy(wrong_range.data() + 400U, &disk, sizeof(disk));
    const auto wrong_range_result = TAssetSerDeser<TextureAsset>::decode(
        fixtureId(),
        lux::cxx::SharedBytes<>::copyOf(wrong_range),
        decode_limits
    );
    assert(!wrong_range_result && wrong_range_result.error().code == EAssetDecodeError::INVALID_PAYLOAD);

    auto unsupported_format = *encoded;
    std::memcpy(&disk, unsupported_format.data() + 400U, sizeof(disk));
    disk.pixel_format = 0xFFFFFFFFU;
    std::memcpy(unsupported_format.data() + 400U, &disk, sizeof(disk));
    const auto unsupported_format_result = TAssetSerDeser<TextureAsset>::decode(
        fixtureId(),
        lux::cxx::SharedBytes<>::copyOf(unsupported_format),
        decode_limits
    );
    assert(!unsupported_format_result &&
        unsupported_format_result.error().code == EAssetDecodeError::INVALID_PAYLOAD);

    std::array<std::uint8_t, 16U> wrong_id_bytes{};
    const auto fixture_id_bytes = fixtureId().bytes();
    for (std::size_t index = 0U; index < wrong_id_bytes.size(); ++index)
        wrong_id_bytes[index] = std::to_integer<std::uint8_t>(fixture_id_bytes[index]);
    wrong_id_bytes[15] = 2U;
    const auto wrong_id_result = TAssetSerDeser<TextureAsset>::decode(
        AssetId{wrong_id_bytes},
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    );
    assert(!wrong_id_result && wrong_id_result.error().code == EAssetDecodeError::ASSET_ID_MISMATCH);

    const auto limited = TAssetSerDeser<TextureAsset>::decode(
        fixtureId(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        AssetDecodeLimits{encoded->size() - 1U, encoded->size(), 0U}
    );
    assert(!limited && limited.error().code == EAssetDecodeError::LIMIT_EXCEEDED);
    return 0;
}
