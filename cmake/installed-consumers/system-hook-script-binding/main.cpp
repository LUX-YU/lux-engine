#include <lux/engine/function/script/BoundScriptCall.hpp>
#include <lux/engine/meta/ScriptMetaAdapter.hpp>
#include <lux/engine/resource/asset/script/ScriptAsset.hpp>
#include <lux/engine/simulation/ScriptBindingSession.hpp>
#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>

#include <array>
#include <cassert>
#include <memory>

namespace
{
    inline constexpr std::array kHooks{
        lux::simulation::makeSystemHookPoint<void()>("update")};
    inline constexpr std::array kEvents{
        lux::simulation::makeSystemEvent<void>(
            "done",
            kHooks[0],
            lux::simulation::ESystemEventTarget::GLOBAL,
            {},
            0U
        )};
    inline constexpr lux::simulation::SystemDescription kSystem{
        .canonical_name = "consumer.system",
        .version = 1U,
        .hooks = kHooks,
        .events = kEvents};
    static_assert(lux::simulation::validSystemDescription(kSystem));
    static_assert(sizeof(lux::script::BoundScriptCall) == 2U * sizeof(void*));
}

int main()
{
    lux::simulation::SimulationDescriptionBuilder builder;
    assert(builder.addSystem("consumer", kSystem));
    auto description = std::move(builder).build();
    assert(description && description->findHookPoint("consumer", "update"));

    lux::asset::ScriptAssetContent script_asset;
    script_asset.description.schema_version = lux::rdesc::Script::kSchemaVersion;
    script_asset.description.module_name = "consumer.script";
    script_asset.description.body = lux::rdesc::CppBehaviorScript{"consumer"};
    script_asset.description.exports.push_back({"update", 1U, {}, {}});
    assert(lux::rdesc::validScriptDescription(script_asset.description));

    const auto script_codec = lux::asset::scriptAssetCodecDescriptor(
        std::make_shared<int>(1)
    );
    const auto simulation_codec = lux::simulation::simulationAssetCodecDescriptor(
        std::make_shared<int>(2)
    );
    assert(script_codec.primary_magic != 0U);
    assert(simulation_codec.primary_magic != 0U);

    lux::simulation::ecs::Registry registry;
    auto session = lux::simulation::ScriptBindingSession::create(
        std::move(*description),
        registry,
        {1U, 1U, 1U, 1U, 1U},
        {},
        {}
    );
    assert(!session);
    return 0;
}
