#include "ConsumerBehavior.hpp"

#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/ScriptSystemDescriptionCodec.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    inline constexpr lux::system::SystemInstanceId SystemId{101U};
    inline constexpr HookPointId ValueHook{102U};
    inline constexpr HookPointId EventHook{103U};
    inline constexpr EventPointId PulseEvent{104U};
    inline constexpr lux::script::ScriptSymbolId ValueSymbol{201U};
    inline constexpr lux::script::ScriptSymbolId EventSymbol{202U};
    inline constexpr lux::script::ScriptApiContractIdView AbilityContract{"consumer.InventoryAbility"};
    inline constexpr std::uint64_t AbilitySchema{0xC011AB1EU};

    inline constexpr std::array Hooks{
        makeHookPointSpec<void(float)>(ValueHook, "value"),
        makeHookPointSpec<void()>(EventHook, "after-event")};
    inline constexpr std::array Events{
        makeEventPointSpec<installed_consumer::CollisionEvent>(
            PulseEvent,
            "pulse",
            EventHook,
            EEventRoute::ENTITY_TARGETED,
            "consumer.CollisionEvent",
            1U)};
    inline constexpr SimulationSystemDescription System{
        .type = {.canonical_name = "consumer.system", .version = 1U},
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

    struct Fixture final
    {
        lux::asset::AssetId asset_id{assetId()};
        lux::world::WorldObjectId object{objectId()};
        std::shared_ptr<const lux::script::ScriptArtifact> artifact;
        lux::simulation::ecs::Entity entity{
            lux::simulation::ecs::NullEntity};
    };

    bool resolveAsset(
        void* context,
        const lux::asset::AssetId& id,
        ResolvedScriptArtifact& result
    ) noexcept
    {
        auto& fixture = *static_cast<Fixture*>(context);
        if (id != fixture.asset_id)
            return false;
        result.artifact = fixture.artifact.get();
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

    bool resolveRecord(
        void*,
        const lux::meta::RefType& type,
        lux::semantic::Layout& result
    ) noexcept
    {
        const auto* reflected = static_cast<const lux::meta::RefClass*>(
            type.ptr
        );
        if (!reflected || reflected->full_name !=
            "installed_consumer::CollisionEvent")
        {
            return false;
        }
        constexpr std::string_view name{"consumer.CollisionEvent"};
        result = {
            lux::semantic::typeId(name),
            name,
            LUX_SCRIPT_VK_STRUCT_REF,
            sizeof(installed_consumer::CollisionEvent),
            alignof(installed_consumer::CollisionEvent)};
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
    const std::array ability_requirements{
        lux::rdesc::ScriptApiRequirement{
            lux::script::ScriptApiContractId{AbilityContract.name()},
            AbilitySchema
        }
    };
    auto projected = projectCppStaticEntityScript(
        "consumer.behavior",
        "consumer-behavior-v1",
        *reflected,
        methods,
        symbols,
        {nullptr, &resolveRecord},
        nullptr,
        {},
        {},
        ability_requirements
    );
    assert(projected);

    Fixture fixture;
    auto projected_description = projected->description();
    auto artifact = lux::script::ScriptArtifact::create(std::move(projected_description), {});
    assert(artifact);
    fixture.artifact = std::make_shared<lux::script::ScriptArtifact>(std::move(*artifact));
    auto artifact_asset = lux::script::ScriptArtifactAsset::create(
        lux::asset::AssetInfo{
            fixture.asset_id,
            lux::script::ScriptArtifactAsset::asset_type,
            0U
        },
        fixture.artifact
    );
    assert(artifact_asset);
    const auto encoded_asset = lux::asset::TAssetSerDeser<
        lux::script::ScriptArtifactAsset>::encode(
        **artifact_asset,
        lux::asset::AssetEncodeLimits{std::numeric_limits<std::size_t>::max()}
    );
    assert(encoded_asset);
    const auto decoded_asset = lux::asset::TAssetSerDeser<
        lux::script::ScriptArtifactAsset>::decode(
        fixture.asset_id,
        lux::cxx::SharedBytes<>::copyOf(*encoded_asset),
        lux::asset::AssetDecodeLimits{
            encoded_asset->size(),
            std::numeric_limits<std::size_t>::max(),
            0U
        }
    );
    assert(decoded_asset);
    fixture.artifact = (*decoded_asset)->sharedData();

    SimulationDescriptionBuilder simulation_builder;
    assert(simulation_builder.addSystem(SystemId, "consumer", System));
    auto simulation = std::move(simulation_builder).build();
    assert(simulation);
    auto simulation_owner = std::make_shared<const SimulationDescription>(std::move(*simulation));

    ScriptSystemDescriptionBuilder script_builder;
    assert(script_builder.addMount({
        ScriptMountId{1U},
        fixture.asset_id,
        EntityScriptMount{fixture.object},
        true,
        {{ValueSymbol, HookScriptTarget{SystemId, ValueHook}},
         {EventSymbol, EventScriptTarget{SystemId, PulseEvent}}}}));
    auto script_description = std::move(script_builder).build(*simulation_owner);
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
        *simulation_owner,
        wire_limits);
    assert(decoded_script);

    auto simulation_asset = SimulationAsset::create(
        lux::asset::AssetInfo{assetId(), SimulationAsset::asset_type, 0U},
        simulation_owner
    );
    assert(simulation_asset);
    const auto encoded_simulation = lux::asset::TAssetSerDeser<SimulationAsset>::encode(
        **simulation_asset,
        lux::asset::AssetEncodeLimits{std::numeric_limits<std::size_t>::max()}
    );
    assert(encoded_simulation);
    assert(lux::asset::TAssetSerDeser<SimulationAsset>::decode(
        (*simulation_asset)->id(),
        lux::cxx::SharedBytes<>::copyOf(*encoded_simulation),
        lux::asset::AssetDecodeLimits{
            encoded_simulation->size(),
            std::numeric_limits<std::size_t>::max(),
            0U
        }
    ));

    ecs::Registry registry;
    fixture.entity = registry.create();
    HookPoint<void(float)> value_hook;
    EventPoint<
        EntityTargetedRoute<ecs::Entity>,
        installed_consumer::CollisionEvent> pulse;
    assert(value_hook.prepare(1U) == EEndpointMutationError::NONE);
    assert(pulse.prepare(1U, 2U, 1U) == EEndpointMutationError::NONE);
    ScriptHookEndpoint<void(float)> hook_bridge{
        SystemId,
        ValueHook,
        value_hook};
    ScriptEventEndpoint<
        EntityTargetedRoute<ecs::Entity>,
        installed_consumer::CollisionEvent>
        event_bridge{SystemId, PulseEvent, pulse};
    const std::array hook_endpoints{hook_bridge.descriptor()};
    const std::array event_endpoints{event_bridge.descriptor()};

    const std::array pools{CppStaticScriptPoolDescription{std::addressof(*projected), 1U}};
    auto backend_result = CppStaticScriptBackend::create(pools);
    assert(backend_result);
    auto backend = std::move(*backend_result);
    const std::array backends{backend.descriptor()};
    const std::uint32_t ability_dispatch{1U};
    const std::array capabilities{ScriptApiCapabilityPublication{
        AbilityContract,
        AbilitySchema,
        nullptr,
        &ability_dispatch
    }};
    SimulationClock clock;
    auto created = ScriptSystem::create(
        *simulation_owner,
        *decoded_script,
        registry,
        clock,
        ScriptRuntimeLimits{2U, 1U, 2U, 2U, 2U, 2U, 64U, 2U, 2U, 2U, 2U},
        {&fixture, &resolveAsset},
        {&fixture, &resolveWorld},
        capabilities,
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
        assert(writer.record(
            fixture.entity,
            installed_consumer::CollisionEvent{17, 4.5F}
        ));
    }
    assert(pulse.drain() == 1U);
    assert(installed_consumer::observed_event.body == 17);
    assert(installed_consumer::observed_event.impulse == 4.5F);
    assert(script_system.shutdown());
    lux::meta::ReflectionRegistry::destroyRegistry();
    return 0;
}
