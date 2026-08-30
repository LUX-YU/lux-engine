#include <lux/cxx/algorithm/sha256.hpp>

#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAtlasAssets.hpp>

#include <Eigen/Core>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace
{
    [[nodiscard]] lux::asset::AssetId id(std::uint32_t ordinal)
    {
        std::array<std::uint8_t, 16U> bytes{
            0x31U, 0x90U, 0x14U, 0x00U,
            0x00U, 0x00U, 0x40U, 0x00U,
            0x80U, 0x00U, 0x00U, 0x00U,
            0x00U, 0x00U, 0x00U, 0x00U,
        };
        bytes[12] = static_cast<std::uint8_t>((ordinal >> 24U) & 0xFFU);
        bytes[13] = static_cast<std::uint8_t>((ordinal >> 16U) & 0xFFU);
        bytes[14] = static_cast<std::uint8_t>((ordinal >> 8U) & 0xFFU);
        bytes[15] = static_cast<std::uint8_t>(ordinal & 0xFFU);
        return lux::asset::AssetId{bytes};
    }

    template <class Asset>
    [[nodiscard]] lux::asset::AssetInfo info(std::uint32_t ordinal)
    {
        lux::asset::AssetInfo result{};
        result.id = id(ordinal);
        result.type = Asset::asset_type;
        result.date = 0x0102030405060708ULL;
        constexpr std::string_view display{"wire-contract"};
        std::memcpy(result.display_name.data(), display.data(), display.size());
        return result;
    }

    [[nodiscard]] std::string sha256(std::span<const std::byte> bytes)
    {
        const auto digest = lux::cxx::algorithm::Sha256::hash(bytes);
        std::array<char, lux::cxx::algorithm::Sha256Digest::hex_size> result{};
        digest.formatHex(result);
        return {result.data(), result.size()};
    }

    template <class Asset>
    void verify(
        const std::shared_ptr<const Asset>& asset,
        std::size_t expected_size,
        std::string_view expected_hash
    )
    {
        constexpr lux::asset::AssetEncodeLimits encode_limits{1024U * 1024U};
        constexpr lux::asset::AssetDecodeLimits decode_limits{1024U * 1024U, 1024U * 1024U, 4U};
        const auto encoded = lux::asset::TAssetSerDeser<Asset>::encode(*asset, encode_limits);
        assert(encoded);
        assert(encoded->size() == expected_size);
        assert(sha256(*encoded) == expected_hash);
        const auto decoded = lux::asset::TAssetSerDeser<Asset>::decode(
            asset->id(),
            lux::cxx::SharedBytes<>::copyOf(*encoded),
            decode_limits
        );
        assert(decoded);
        const auto reencoded = lux::asset::TAssetSerDeser<Asset>::encode(**decoded, encode_limits);
        assert(reencoded && *reencoded == *encoded);
    }
} // namespace

int main()
{
    auto model = std::make_shared<lux::rdesc::ModelDescription>();
    model->primitives.push_back({id(6U), id(4U)});
    model->nodes.push_back({});
    model->nodes.front().local_transform.translation() = Eigen::Vector3f{1.0F, 2.0F, 3.0F};
    model->nodes.front().primitives.push_back(0U);
    model->skeleton = id(10U);
    model->animations.push_back(id(11U));
    const auto model_asset = lux::asset::ModelAsset::create(
        info<lux::asset::ModelAsset>(7U),
        std::move(model)
    );
    assert(model_asset);
    verify(
        *model_asset,
        564U,
        "47e4ab994c3d3666cd34e92f5fc4b2e499a04bdf5d94a0940f13582c8b5a6215"
    );

    auto atlas = std::make_shared<lux::rdesc::TextureAtlas>();
    atlas->name = "atlas";
    atlas->texture = id(1U);
    atlas->frames.push_back({
        "idle",
        Eigen::Vector4f{0.0F, 0.0F, 0.5F, 1.0F},
        Eigen::Vector2f{0.25F, 0.75F},
    });
    const auto atlas_asset = lux::asset::TextureAtlasAsset::create(
        info<lux::asset::TextureAtlasAsset>(2U),
        std::move(atlas)
    );
    assert(atlas_asset);
    verify(
        *atlas_asset,
        477U,
        "2c0a7f6353760c6994065c143b169707c16191899076b53c0814604e5a86d2e1"
    );

    auto clip = std::make_shared<lux::rdesc::FlipbookClip>();
    clip->name = "blink";
    clip->atlas = id(2U);
    clip->frames.push_back({0U, 0.125F});
    clip->events.push_back({0U, 7U});
    clip->loop = false;
    const auto clip_asset = lux::asset::FlipbookClipAsset::create(
        info<lux::asset::FlipbookClipAsset>(3U),
        std::move(clip)
    );
    assert(clip_asset);
    verify(
        *clip_asset,
        469U,
        "38e7fa62a043f95947ba06b0a756118ec86ea33250195791038e541747a15533"
    );
    return 0;
}
