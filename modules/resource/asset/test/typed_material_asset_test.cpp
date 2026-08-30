#include <lux/cxx/algorithm/sha256.hpp>

#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/CookedAssetImage.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

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
    std::vector<std::byte> verify(
        const std::shared_ptr<const Asset>& asset,
        std::size_t expected_size,
        std::string_view expected_hash
    )
    {
        constexpr lux::asset::AssetEncodeLimits encode_limits{1024U * 1024U};
        constexpr lux::asset::AssetDecodeLimits decode_limits{1024U * 1024U, 1024U * 1024U, 4U};
        const auto encoded = lux::asset::TAssetSerDeser<Asset>::encode(*asset, encode_limits);
        assert(encoded && encoded->size() == expected_size);
        const auto actual_hash = sha256(*encoded);
        if (actual_hash != expected_hash)
            std::cerr << "material golden mismatch: size=" << encoded->size() << " sha256=" << actual_hash << '\n';
        assert(actual_hash == expected_hash);
        const auto decoded = lux::asset::TAssetSerDeser<Asset>::decode(
            asset->id(),
            lux::cxx::SharedBytes<>::copyOf(*encoded),
            decode_limits
        );
        assert(decoded);
        const auto reencoded = lux::asset::TAssetSerDeser<Asset>::encode(**decoded, encode_limits);
        assert(reencoded && *reencoded == *encoded);
        return *encoded;
    }

    template <class Asset, class Value>
    void rejectInformationMutation(
        std::vector<std::byte> encoded,
        lux::asset::AssetId requested,
        std::size_t relative_offset,
        const Value& value
    )
    {
        constexpr lux::asset::AssetDecodeLimits limits{1024U * 1024U, 1024U * 1024U, 4U};
        auto owned = lux::cxx::SharedBytes<>::copyOf(encoded);
        const auto image = lux::asset::inspectCookedAssetImage(owned, limits);
        assert(image);
        const auto information_offset = static_cast<std::size_t>(
            image->information().data() - image->image().data()
        );
        std::memcpy(encoded.data() + information_offset + relative_offset, &value, sizeof(Value));
        const auto decoded = lux::asset::TAssetSerDeser<Asset>::decode(
            requested,
            lux::cxx::SharedBytes<>::copyOf(encoded),
            limits
        );
        assert(!decoded);
    }
} // namespace

int main()
{
    auto material = std::make_shared<lux::rdesc::MaterialDescription>();
    material->parameter_count = 1U;
    material->parameter_defaults[0] = {0.25F, 0.5F, 0.75F, 1.0F};
    material->alpha_mode = lux::rdesc::EAlphaMode::Blend;
    material->double_sided = true;
    material->gbuffer_spirv = {0x07230203U, 0x00010000U, 0U, 1U, 0U};
    material->forward_spirv = {0x07230203U, 0x00010000U, 0U, 1U, 0U};
    material->texture_slot_ids[0] = id(1U);
    const auto material_asset = lux::asset::MaterialAsset::create(
        info<lux::asset::MaterialAsset>(4U),
        std::move(material)
    );
    assert(material_asset);
    const auto material_wire = verify(
        *material_asset,
        519U,
        "f6eea4f51c2f6b1f4fd1cc5c5b7943f06ba339430aaf2c70cb0962157730d5f6"
    );
    rejectInformationMutation<lux::asset::MaterialAsset>(material_wire, id(4U), 24U, 99U);

    auto invalid_material = std::make_shared<lux::rdesc::MaterialDescription>((*material_asset)->data());
    invalid_material->gbuffer_spirv.resize(1U);
    assert(!lux::asset::MaterialAsset::create(
        info<lux::asset::MaterialAsset>(40U),
        invalid_material
    ));

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

    auto invalid_instance = std::make_shared<lux::rdesc::MaterialInstanceDescription>();
    invalid_instance->parent_material_id = id(4U);
    invalid_instance->tex_override_mask = 1U;
    assert(!lux::asset::MaterialInstanceAsset::create(
        info<lux::asset::MaterialInstanceAsset>(50U),
        invalid_instance
    ));
    invalid_instance->texture_slot_ids[0] = id(1U);
    invalid_instance->param_override_mask = 1U << lux::rdesc::MaterialInstanceDescription::kMaxParams;
    assert(!lux::asset::MaterialInstanceAsset::create(
        info<lux::asset::MaterialInstanceAsset>(51U),
        invalid_instance
    ));
    return 0;
}
