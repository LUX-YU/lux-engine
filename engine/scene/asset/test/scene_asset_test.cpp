#include <lux/cxx/algorithm/sha256.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/scene/SceneAssetCodec.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>

namespace
{
    [[nodiscard]] lux::asset::AssetId id(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[15] = tail;
        return lux::asset::AssetId(bytes);
    }
}

int main()
{
    using namespace lux;
    constexpr asset::AssetEncodeLimits encode_limits{1024U};
    constexpr asset::AssetDecodeLimits decode_limits{1024U, 1024U, 4U};
    const auto description = std::make_shared<const scene::SceneDescription>(
        scene::SceneDescription{id(1U), id(2U)}
    );
    const auto value = scene::SceneAsset::create(
        asset::AssetInfo{id(3U), scene::SceneAsset::asset_type, 31U},
        description
    );
    assert(value);
    const auto encoded = asset::TAssetSerDeser<scene::SceneAsset>::encode(**value, encode_limits);
    assert(encoded && encoded->size() == 440U);

    const auto outer = asset::inspectCookedAssetImage(
        (*value)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    );
    assert(outer && outer->information().empty() && outer->data().size() == 40U);
    const auto digest = lux::cxx::algorithm::Sha256::hash(outer->data().view());
    const auto expected = lux::cxx::algorithm::Sha256Digest::fromHex(
        "ff5add560504767ee622029d7c794ff8635749525df53d03be7d7dbcb97af6cd"
    );
    assert(expected && digest == *expected);

    const auto decoded = asset::TAssetSerDeser<scene::SceneAsset>::decode(
        (*value)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    );
    assert(decoded);
    assert((*decoded)->data().world == description->world);
    assert((*decoded)->data().simulation == description->simulation);
    assert((*decoded)->as<scene::SceneAsset>() == decoded->get());

    auto trailing = *encoded;
    trailing.push_back(std::byte{});
    assert(!asset::TAssetSerDeser<scene::SceneAsset>::decode(
        (*value)->id(),
        lux::cxx::SharedBytes<>::copyOf(trailing),
        asset::AssetDecodeLimits{trailing.size(), trailing.size(), 4U}
    ));

    auto corrupt = *encoded;
    corrupt[400U] ^= std::byte{1U};
    assert(!asset::TAssetSerDeser<scene::SceneAsset>::decode(
        (*value)->id(),
        lux::cxx::SharedBytes<>::copyOf(corrupt),
        decode_limits
    ));
    assert(!asset::TAssetSerDeser<scene::SceneAsset>::decode(
        id(4U),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    ));
    return 0;
}
