#include "ConsumerBehavior.hpp"

#include <lux/engine/resource/asset/script/ScriptAsset.hpp>
#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/script/ScriptSystem.hpp>
#include <lux/engine/simulation/script/ScriptSystemDescriptionCodec.hpp>
#include <lux/engine/simulation/script/cpp_static/CppStaticScriptBridge.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    inline constexpr SystemInstanceId SystemId{101U};
    inline constexpr HookPointId ValueHook{102U};
    inline constexpr HookPointId EventHook{103U};
    inline constexpr EventPointId PulseEvent{104U};
    inline constexpr lux::script::ScriptSymbolId ValueSymbol{201U};
    inline constexpr lux::script::ScriptSymbolId EventSymbol{202U};

    inline constexpr std::array Hooks{
        makeHookPointSpec<void(float)>(ValueHook, "value"),
        makeHookPointSpec<void()>(EventHook, "after-event")};
    inline constexpr std::array Events{
        makeEventPointSpec<std::int32_t>(
            PulseEvent,
            "pulse",
            EventHook,
            EEventRoute::ENTITY_TARGETED,
            "lux.i32",
            1U)};
    inline constexpr SystemDescription System{
        .canonical_name = "consumer.system",
        .version = 1U,
        .hooks = Hooks,
        .events = Events};

    [[nodiscard]] lux::asset::AssetId assetId()
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = 0xC4U;
        return lux::asset::AssetId{bytes};
    }

    [[nodiscard]] lux::world::WorldObjectId objectId()
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = 0xD5U;
        return lux::world::WorldObjectId{uuids::uuid{bytes}};
    }

    [[nodiscard]] lux::asset::AssetCodecLimits unlimited()
    {
        return {
            std::numeric_limits<std::size_t>::max(),
            std::numeric_limits<std::size_t>::max(),
            std::numeric_limits<std::size_t>::max()};
    }

    struct Fixture final
    {
        lux::asset::AssetId asset_id{assetId()};
        lux::world::WorldObjectId object{objectId()};
        lux::asset::ScriptAssetContent asset;
        lux::simulation::ecs::Entity entity{
            lux::simulation::ecs::NullEntity};
    };

    bool resolveAsset(
        void* context,
        const lux::asset::AssetId& id,
        ResolvedScriptAsset& result
    ) noexcept
    {
        auto& fixture = *static_cast<Fixture*>(context);
        if (id != fixture.asset_id)
            return false;
        result.asset = std::addressof(fixture.asset);
        return true;
    }

    bool resolveWorld(
        void* context,
        const lux::world::WorldObjectId& object,
        lux::simulation::ecs::Entity& result
    ) noexcept
    {
        auto& fixture = *static_cast<Fixture*>(context);
        if (object != fixture.object)
            return false;
        result = fixture.entity;
        return true;
    }
}

int main()
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    lux::meta::ReflectionRegistry::initRegistry();
    auto& reflection = lux::meta::ReflectionRegistry::instance();
    const auto* reflected = reflection.findClass(
        "installed_consumer::ConsumerBehavior");
    assert(reflected && reflected->methods.size() == 2U);
    const lux::meta::RefMethod* value_method{};
    const lux::meta::RefMethod* event_method{};
    for (const auto& method : reflected->methods)
    {
        if (method.invokable.name == "onValue")
            value_method = &method;
        else if (method.invokable.name == "onEvent")
            event_method = &method;
    }
    assert(value_method && event_method);
    const std::array methods{value_method, event_method};
    const std::array symbols{ValueSymbol, EventSymbol};
    auto projected = projectCppStaticEntityScript(
        "consumer.behavior",
        "consumer-behavior-v1",
        *reflected,
        methods,
        symbols,
        {});
    assert(projected);

    Fixture fixture;
    fixture.asset.description = projected->description();
    const auto asset_codec = lux::asset::scriptAssetCodecDescriptor({});
    const auto encoded_asset = asset_codec.encode(
        std::addressof(fixture.asset),
        lux::asset::AssetEncodeContext{unlimited()});
    assert(encoded_asset && (*encoded_asset)[4] == std::byte{3U});
    const auto decoded_asset = asset_codec.decode(
        *encoded_asset,
        lux::asset::AssetDecodeContext{unlimited()});
    assert(decoded_asset);
    fixture.asset = *std::static_pointer_cast<
        const lux::asset::ScriptAssetContent>(decoded_asset->payload);

    SimulationDescriptionBuilder simulation_builder;
    assert(simulation_builder.addSystem(SystemId, "consumer", System));
    auto simulation = std::move(simulation_builder).build();
    assert(simulation);

    ScriptSystemDescriptionBuilder script_builder;
    assert(script_builder.addMount({
        ScriptMountId{1U},
        fixture.asset_id,
        EntityScriptMount{fixture.object},
        true,
        {{ValueSymbol, HookScriptTarget{SystemId, ValueHook}},
         {EventSymbol, EventScriptTarget{SystemId, PulseEvent}}}}));
    auto script_description = std::move(script_builder).build(*simulation);
    assert(script_description);
    const ScriptSystemCodecLimits wire_limits{
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max()};
    const auto encoded_script = encodeScriptSystemDescription(
        *script_description,
        wire_limits);
    assert(encoded_script);
    const auto decoded_script = decodeScriptSystemDescription(
        *encoded_script,
        *simulation,
        wire_limits);
    assert(decoded_script);

    const auto simulation_codec = simulationAssetCodecDescriptor({});
    const auto encoded_simulation = simulation_codec.encode(
        std::addressof(*simulation),
        lux::asset::AssetEncodeContext{unlimited()});
    assert(encoded_simulation && (*encoded_simulation)[4] == std::byte{5U});
    assert(simulation_codec.decode(
        *encoded_simulation,
        lux::asset::AssetDecodeContext{unlimited()}));

    ecs::Registry registry;
    fixture.entity = registry.create();
    HookPoint<void(float)> value_hook;
    EventPoint<EntityTargetedRoute<ecs::Entity>, std::int32_t> pulse;
    assert(value_hook.prepare(1U) == EEndpointMutationError::NONE);
    assert(pulse.prepare(1U, 2U, 1U) == EEndpointMutationError::NONE);
    ScriptHookEndpoint<void(float)> hook_bridge{
        SystemId,
        ValueHook,
        value_hook};
    ScriptEventEndpoint<EntityTargetedRoute<ecs::Entity>, std::int32_t>
        event_bridge{SystemId, PulseEvent, pulse};
    const std::array hook_endpoints{hook_bridge.descriptor()};
    const std::array event_endpoints{event_bridge.descriptor()};

    const std::array descriptors{std::addressof(*projected)};
    CppStaticScriptBackend backend{descriptors, 1U};
    assert(backend);
    const std::array backends{backend.descriptor()};
    auto created = ScriptSystem::create(
        *simulation,
        *decoded_script,
        registry,
        ScriptSystemCapacities{1U, 2U, 1U, 1U, 2U, 2U, 2U},
        {&fixture, &resolveAsset},
        {&fixture, &resolveWorld},
        backends,
        hook_endpoints,
        event_endpoints);
    assert(created);
    auto script_system = std::move(*created);
    assert(script_system.prepare());
    assert(script_system.activeInstanceCount() == 1U);

    assert(value_hook.dispatch(2.5F) == 1U);
    assert(installed_consumer::observed_value == 2.5F);
    {
        auto writer = pulse.begin(0U);
        assert(writer.record(fixture.entity, 17));
    }
    assert(pulse.drain() == 1U);
    assert(installed_consumer::observed_event == 17);
    assert(script_system.shutdown());
    lux::meta::ReflectionRegistry::destroyRegistry();
    return 0;
}
