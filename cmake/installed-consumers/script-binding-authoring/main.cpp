#include <lux/engine/authoring/ScriptBindingAuthoring.hpp>
#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace
{
    using namespace lux::simulation;

    inline constexpr std::array kHooks{
        makeSystemHookPoint<void(float)>("first", ESystemHookCardinality::MULTI),
        makeSystemHookPoint<void(float)>("second", ESystemHookCardinality::MULTI)};
    inline constexpr SystemDescription kSystem{
        .canonical_name = "installed.authoring.system",
        .version = 1U,
        .hooks = kHooks};

    lux::asset::AssetCodecLimits unlimited()
    {
        return {
            std::numeric_limits<std::size_t>::max(),
            std::numeric_limits<std::size_t>::max(),
            std::numeric_limits<std::size_t>::max()};
    }
}

int
main()
{
    using namespace lux::simulation;

    SimulationDescriptionBuilder target_builder;
    assert(target_builder.addSystem("installed", kSystem));
    auto targets = std::move(target_builder).build();
    assert(targets);

    lux::rdesc::Script script;
    script.module_name = "installed.authoring.script";
    script.model = lux::rdesc::EScriptModel::GLOBAL_MODULE;
    script.body = lux::rdesc::CppStaticScript{"installed-authoring-v1"};
    constexpr lux::script::ScriptSymbolId symbol{77U};
    script.exports.push_back(lux::rdesc::ScriptFunction{
        "apply",
        symbol,
        {{"lux.f32", lux::script::scriptSemanticTypeId("lux.f32"), lux::script::EScriptPassMode::VALUE}},
        {}}
    );
    assert(lux::rdesc::validScriptDescription(script));

    const auto catalog = lux::authoring::makeScriptBindingTargetCatalog(*targets);
    const auto compatible = lux::authoring::compatibleScriptBindingTargets(script, symbol, catalog);
    assert(catalog.size() == 7U);
    assert(compatible.size() == 2U);

    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes[0] = 0xA8U;
    ScriptMountDescription mount{ScriptMountId{1U}, lux::asset::AssetId{id_bytes}, {}};
    for (const auto index : compatible)
    {
        assert(
            lux::authoring::addScriptBinding(
                *targets,
                script,
                mount,
                ScriptBindingDescription{symbol, catalog[index].target}) ==
            lux::authoring::EScriptBindingAuthoringError::SUCCESS);
    }
    assert(mount.bindings.size() == 2U);
    assert(lux::authoring::diagnoseScriptBindings(*targets, script, mount).empty());
    const std::array composition{lux::authoring::ScriptBindingCompositionEntry{&script, &mount}};
    assert(lux::authoring::diagnoseScriptBindingComposition(*targets, composition).empty());

    SimulationDescriptionBuilder final_builder;
    assert(final_builder.addSystem("installed", kSystem));
    assert(final_builder.addGlobalScriptMount(mount));
    auto description = std::move(final_builder).build();
    assert(description);
    const auto codec = simulationAssetCodecDescriptor({});
    const auto encoded = codec.encode(std::addressof(*description), lux::asset::AssetEncodeContext{unlimited()});
    assert(encoded && (*encoded)[4] == std::byte{4U});
    const auto decoded = codec.decode(*encoded, lux::asset::AssetDecodeContext{unlimited()});
    assert(decoded);
    const auto restored = std::static_pointer_cast<const SimulationDescription>(decoded->payload);
    assert(restored->globalScriptMountCount() == 1U);
    assert(restored->globalScriptMountAt(0U).bindingCount() == 2U);
}
