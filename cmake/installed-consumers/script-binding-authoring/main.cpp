#include <lux/engine/authoring/script/ScriptAuthoring.hpp>
#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ScriptSystemDescriptionCodec.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace
{
    using namespace lux::simulation;

    inline constexpr lux::system::SystemInstanceId SystemId{301U};
    inline constexpr HookPointId FirstHook{302U};
    inline constexpr HookPointId SecondHook{303U};
    inline constexpr std::array Hooks{
        makeHookPointSpec<void(float)>(FirstHook, "first"),
        makeHookPointSpec<void(float)>(SecondHook, "second")};
    inline constexpr SimulationSystemDescription System{
        .type = {.canonical_name = "installed.authoring.system", .version = 1U},
        .hooks = Hooks};

}

int main()
{
    using namespace lux::authoring::script;
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    SimulationDescriptionBuilder target_builder;
    assert(target_builder.addSystem(SystemId, "installed", System));
    auto targets = std::move(target_builder).build();
    assert(targets);

    constexpr lux::script::ScriptSymbolId Symbol{77U};
    lux::rdesc::Script asset;
    asset.module_name = "installed.authoring.script";
    asset.body = lux::rdesc::CppStaticScript{"installed-authoring-v1"};
    asset.exports.push_back({
        "apply",
        Symbol,
        {lux::rdesc::makeScriptValueType<float>()},
        {}});
    assert(lux::rdesc::validScriptDescription(asset));

    const auto compatible = compatibleHookTargets(
        *targets,
        asset.exports[0]);
    assert(compatible.size() == 2U);
    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes[0] = 0xA8U;
    ScriptMountDescription mount{
        ScriptMountId{1U},
        lux::asset::AssetId{id_bytes},
        SimulationScriptMount{},
        true,
        {}};
    for (const auto target : compatible)
    {
        assert(addBinding(
            *targets,
            asset,
            mount,
            {Symbol, target}) == EScriptAuthoringError::SUCCESS);
    }
    assert(mount.bindings.size() == 2U);
    assert(diagnoseBindings(*targets, asset, mount).empty());

    ScriptSystemDescriptionBuilder script_builder;
    assert(script_builder.addMount(std::move(mount)));
    auto authored = std::move(script_builder).build(*targets);
    assert(authored);
    const ScriptSystemCodecLimits limits{
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max()};
    const auto encoded_authored = encodeScriptSystemDescription(
        *authored,
        limits);
    assert(encoded_authored);
    const auto decoded_authored = decodeScriptSystemDescription(
        *encoded_authored,
        *targets,
        limits);
    assert(decoded_authored && decoded_authored->mounts().size() == 1U);
    assert(decoded_authored->mounts()[0].bindings.size() == 2U);

    SimulationDescriptionBuilder final_builder;
    assert(final_builder.addSystem(SystemId, "installed", System));
    assert(addScriptSystemData(final_builder, *authored, limits));
    auto description = std::move(final_builder).build();
    assert(description);
    auto description_owner = std::make_shared<const SimulationDescription>(std::move(*description));
    std::array<std::uint8_t, 16U> simulation_asset_id{};
    simulation_asset_id.back() = 9U;
    auto simulation_asset = SimulationAsset::create(
        lux::asset::AssetInfo{
            lux::asset::AssetId{simulation_asset_id},
            SimulationAsset::asset_type,
            0U
        },
        description_owner
    );
    assert(simulation_asset);
    const auto encoded = lux::asset::TAssetSerDeser<SimulationAsset>::encode(
        **simulation_asset,
        lux::asset::AssetEncodeLimits{std::numeric_limits<std::size_t>::max()}
    );
    assert(encoded);
    const auto decoded = lux::asset::TAssetSerDeser<SimulationAsset>::decode(
        (*simulation_asset)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded),
        lux::asset::AssetDecodeLimits{
            encoded->size(),
            std::numeric_limits<std::size_t>::max(),
            0U
        }
    );
    assert(decoded);
    const auto data = (*decoded)->data().findData(scriptSystemDataSchemaId());
    assert(data && data.version() == ScriptSystemDescription::kSchemaVersion);
    return 0;
}
