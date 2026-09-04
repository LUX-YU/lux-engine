#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/resource/asset/CookedAssetImage.hpp>

#include <array>
#include <cassert>
#include <limits>
#include <memory>
#include <vector>

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
    description.event_requirements.push_back({
        "Gameplay",
        "damage",
        21U,
        22U,
        script::EScriptEventRoute::SIMULATION_BROADCAST,
        {
            "lux.i32",
            semantic::typeId("lux.i32"),
            static_cast<std::uint8_t>(semantic::EAbiKind::I32),
            4U,
            4U
        },
        semantic::typeId("lux.i32"),
        1U
    });
    description.body = rdesc::LuaSourceScript{"module", {11U}};
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
    assert(image && image->data().view()[4] == std::byte{9U});

    auto decoded = asset::TAssetSerDeser<script::ScriptArtifactAsset>::decode(
        (*typed)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        decode_limits
    );
    assert(decoded);
    const auto& artifact = (*decoded)->data();
    assert(artifact.description().schema_version == 11U);
    assert(artifact.description().lifecycle.begin_play == 10U);
    assert(artifact.description().lifecycle.end_play == 12U);
    assert(artifact.description().api_requirements.size() == 1U);
    assert(artifact.description().api_requirements.front().contract.name() == "lux.test.Ability");
    assert(artifact.description().api_requirements.front().expected_schema_hash == 0x11223344U);
    assert(artifact.description().exports[1].symbol_id == 11U);
    assert(std::get<rdesc::LuaSourceScript>(artifact.description().body).suspension_capable_exports ==
        std::vector<script::ScriptSymbolId>{11U});
    const auto& event_sources = artifact.description().event_requirements;
    assert(event_sources.size() == 1U && event_sources.front().system_name == "Gameplay" &&
        event_sources.front().event_name == "damage" && event_sources.front().system_id == 21U &&
        event_sources.front().event_id == 22U);
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

    rdesc::Script cpp_description;
    cpp_description.module_name = "lux.test.cpp-coroutine";
    cpp_description.exports.push_back({"task", 31U, {}, {}});
    cpp_description.body = rdesc::CppStaticScript{"cpp-coroutine-v1", {31U}};
    auto cpp_artifact = script::ScriptArtifact::create(std::move(cpp_description), {});
    assert(cpp_artifact);
    id_bytes.back() = 2U;
    auto cpp_asset = script::ScriptArtifactAsset::create(
        asset::AssetInfo{
            asset::AssetId{id_bytes},
            script::ScriptArtifactAsset::asset_type,
            0U
        },
        std::make_shared<const script::ScriptArtifact>(std::move(*cpp_artifact))
    );
    assert(cpp_asset);
    auto cpp_encoded = asset::TAssetSerDeser<script::ScriptArtifactAsset>::encode(**cpp_asset, encode_limits);
    assert(cpp_encoded);
    auto cpp_decoded = asset::TAssetSerDeser<script::ScriptArtifactAsset>::decode(
        (*cpp_asset)->id(),
        lux::cxx::SharedBytes<>::copyOf(*cpp_encoded),
        decode_limits
    );
    assert(cpp_decoded);
    const auto& cpp_body = std::get<rdesc::CppStaticScript>((*cpp_decoded)->data().description().body);
    assert(cpp_body.descriptor == "cpp-coroutine-v1");
    assert(cpp_body.suspension_capable_exports == std::vector<script::ScriptSymbolId>{31U});
    return 0;
}
