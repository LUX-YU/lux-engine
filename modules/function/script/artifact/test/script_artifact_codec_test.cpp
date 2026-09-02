#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/resource/asset/CookedAssetImage.hpp>

#include <array>
#include <cassert>
#include <limits>
#include <memory>

int main()
{
    using namespace lux;

    rdesc::Script description;
    description.module_name = "lux.test.artifact";
    description.exports.push_back({"begin", 10U, {}, {}});
    description.exports.push_back({"tick", 11U, {}, {}});
    description.exports.push_back({"end", 12U, {}, {}});
    description.lifecycle = {10U, 12U};
    description.api_requirements.push_back({
        script::ScriptApiContractId{"lux.test.Ability"},
        0x11223344U
    });
    description.body = rdesc::LuaSourceScript{"module"};
    auto source_result = script::ScriptArtifact::create(
        std::move(description),
        {std::byte{1U}, std::byte{2U}, std::byte{3U}}
    );
    assert(source_result);
    auto source = std::move(*source_result);
    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes.back() = 1U;
    auto typed = script::ScriptArtifactAsset::create(
        asset::AssetInfo{
            asset::AssetId{id_bytes},
            script::ScriptArtifactAsset::asset_type,
            0U
        },
        std::make_shared<const script::ScriptArtifact>(std::move(source))
    );
    assert(typed);
    constexpr asset::AssetEncodeLimits encode_limits{1024U * 1024U};
    constexpr asset::AssetDecodeLimits decode_limits{1024U * 1024U, 1024U * 1024U, 4U};
    auto encoded = asset::TAssetSerDeser<script::ScriptArtifactAsset>::encode(**typed, encode_limits);
    assert(encoded);
    const auto image = asset::inspectCookedAssetImage(
        (*typed)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    );
    assert(image && image->data().view()[4] == std::byte{5U});

    auto decoded = asset::TAssetSerDeser<script::ScriptArtifactAsset>::decode(
        (*typed)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    );
    assert(decoded);
    const auto& artifact = (*decoded)->data();
    assert(artifact.description().schema_version == 7U);
    assert(artifact.description().lifecycle.begin_play == 10U);
    assert(artifact.description().lifecycle.end_play == 12U);
    assert(artifact.description().api_requirements.size() == 1U);
    assert(artifact.description().api_requirements.front().contract.name() == "lux.test.Ability");
    assert(artifact.description().api_requirements.front().expected_schema_hash == 0x11223344U);
    assert(artifact.description().exports[1].symbol_id == 11U);
    assert(artifact.payload().size() == 3U);
    assert(artifact.findExport(11U) == std::addressof(artifact.description().exports[1]));
    assert(artifact.findExport(12U) == std::addressof(artifact.description().exports[2]));
    assert(artifact.findExport(13U) == nullptr);

    auto old_schema = *encoded;
    old_schema[408U] = std::byte{3U};
    assert(!asset::TAssetSerDeser<script::ScriptArtifactAsset>::decode(
        (*typed)->id(), lux::cxx::SharedBytes<>::copyOf(old_schema), decode_limits
    ));
    auto old_wire = *encoded;
    old_wire[404U] = std::byte{1U};
    assert(!asset::TAssetSerDeser<script::ScriptArtifactAsset>::decode(
        (*typed)->id(), lux::cxx::SharedBytes<>::copyOf(old_wire), decode_limits
    ));
    auto corrupt_kind = *encoded;
    corrupt_kind[412U] = std::byte{0xFFU};
    assert(!asset::TAssetSerDeser<script::ScriptArtifactAsset>::decode(
        (*typed)->id(), lux::cxx::SharedBytes<>::copyOf(corrupt_kind), decode_limits
    ));
    assert(!asset::TAssetSerDeser<script::ScriptArtifactAsset>::decode(
        (*typed)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        asset::AssetDecodeLimits{encoded->size(), 1U, 0U}
    ));
    auto trailing = *encoded;
    trailing.push_back(std::byte{});
    assert(!asset::TAssetSerDeser<script::ScriptArtifactAsset>::decode(
        (*typed)->id(),
        lux::cxx::SharedBytes<>::copyOf(trailing),
        asset::AssetDecodeLimits{trailing.size(), trailing.size(), 4U}
    ));
    return 0;
}
