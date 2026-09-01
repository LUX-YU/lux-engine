#include <lux/cxx/algorithm/sha256.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/scene/SceneAssetCodec.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>

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
    constexpr asset::AssetEncodeLimits encode_limits{4096U};
    constexpr asset::AssetDecodeLimits decode_limits{4096U, 4096U, 4U};
    scene::SceneDescriptionBuilder builder;
    builder.setWorld(id(1U));
    builder.setSimulation(id(2U));
    constexpr system::SystemInstanceId First{10U};
    constexpr system::SystemInstanceId Second{20U};
    const auto first_type = system::systemTypeId("lux.scene.asset.first");
    const auto second_type = system::systemTypeId("lux.scene.asset.second");
    const std::array configuration{std::byte{3U}, std::byte{4U}};
    assert(builder.addSystem(First, "first", first_type, 1U, "lux.scene.asset.Config", 1U, configuration));
    assert(builder.addSystem(Second, "second", second_type, 2U, {}, 0U));
    assert(builder.bindRequirement(First, "runtime", "host.render"));
    assert(builder.addDependency(First, Second));
    auto built = std::move(builder).build();
    assert(built);
    const auto description = std::make_shared<const scene::SceneDescription>(std::move(*built));
    const auto value = scene::SceneAsset::create(
        asset::AssetInfo{id(3U), scene::SceneAsset::asset_type, 31U},
        description
    );
    assert(value);
    const auto encoded = asset::TAssetSerDeser<scene::SceneAsset>::encode(**value, encode_limits);
    assert(encoded);

    const auto outer = asset::inspectCookedAssetImage(
        (*value)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    );
    assert(outer && outer->information().empty() && outer->data().size() == 276U);
    const auto digest = lux::cxx::algorithm::Sha256::hash(outer->data().view());
    const auto expected = lux::cxx::algorithm::Sha256Digest::fromHex(
        "990666a328e50f4f65592b822b8d09f8bead3823de41dafd55890af20bc599b0"
    );
    assert(expected && digest == *expected);

    const auto decoded = asset::TAssetSerDeser<scene::SceneAsset>::decode(
        (*value)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    );
    assert(decoded);
    assert((*decoded)->data().world() == description->world());
    assert((*decoded)->data().simulation() == description->simulation());
    assert((*decoded)->data().systemCount() == 2U);
    assert((*decoded)->data().findSystem(First).configurationPayload().size() == 2U);
    assert((*decoded)->data().findSystem(First).findRequirementBinding("runtime").provider() == "host.render");
    assert((*decoded)->data().dependencyAt(0U).before() == First);
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
    auto old_version = *encoded;
    old_version[404U] = std::byte{1U};
    assert(!asset::TAssetSerDeser<scene::SceneAsset>::decode(
        (*value)->id(),
        lux::cxx::SharedBytes<>::copyOf(old_version),
        decode_limits
    ));
    assert(!asset::TAssetSerDeser<scene::SceneAsset>::decode(
        id(4U),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    ));
    return 0;
}
