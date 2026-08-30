#include <lux/cxx/algorithm/sha256.hpp>

#include <lux/engine/resource/asset/material/MaterialAssets.hpp>

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
        bytes[15] = static_cast<std::uint8_t>(ordinal);
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
        assert(encoded && encoded->size() == expected_size);
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
    auto material = std::make_shared<lux::rdesc::MaterialDescription>();
    material->parameter_count = 1U;
    material->parameter_defaults[0] = {0.25F, 0.5F, 0.75F, 1.0F};
    material->alpha_mode = lux::rdesc::EAlphaMode::Blend;
    material->double_sided = true;
    material->gbuffer_spirv = {0x07230203U, 0x00010000U};
    material->forward_spirv = {0x07230203U, 0x00010001U};
    material->texture_slot_ids[0] = id(1U);
    const auto material_asset = lux::asset::MaterialAsset::create(
        info<lux::asset::MaterialAsset>(4U),
        std::move(material)
    );
    assert(material_asset);
    verify(
        *material_asset,
        495U,
        "34ddba8c3a78463d048553fa3d44481a737646895b6797e8fb62e19e9bd1fd8f"
    );

    auto instance = std::make_shared<lux::rdesc::MaterialInstanceDescription>();
    instance->parent_material_id = id(4U);
    instance->param_override_mask = 1U;
    instance->params[0][0] = 0.5F;
    instance->tex_override_mask = 1U;
    instance->texture_slot_ids[0] = id(1U);
    instance->render_state_override = 1U;
    instance->alpha_mode = lux::rdesc::EAlphaMode::Blend;
    instance->double_sided = true;
    const auto instance_asset = lux::asset::MaterialInstanceAsset::create(
        info<lux::asset::MaterialInstanceAsset>(5U),
        std::move(instance)
    );
    assert(instance_asset);
    verify(
        *instance_asset,
        472U,
        "a35ae4037601ccabd656b458ce979beec7947ccb3081345ddb3955fabbb6d495"
    );
    return 0;
}
