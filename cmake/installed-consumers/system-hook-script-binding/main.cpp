#include "ConsumerBehavior.hpp"
#include "ConsumerBehavior.HookBehavior.script.generated.hpp"
#include "InventoryAbility.ability.generated.hpp"

#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
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

    inline constexpr std::array Hooks{
        makeHookPointSpec<void(float)>(ValueHook, "value"),
        makeHookPointSpec<void()>(EventHook, "after-event", true, true)};
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

    struct Domain final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr auto Description = System;
        std::int32_t count() noexcept { return 1; }
        Fixture& fixture;
        HookPoint<void(float)> value_hook;
        using Channel = HookChannel<EntityTargetedRoute<ecs::Entity>, installed_consumer::CollisionEvent>;
        Channel& pulse;
        Channel::Producer pulse_writer;
        ScriptHookEndpoint<void(float)> hook_bridge{SystemId, ValueHook, value_hook};
        ScriptEventEndpoint<EntityTargetedRoute<ecs::Entity>, installed_consumer::CollisionEvent>
            event_bridge{SystemId, PulseEvent, pulse};
        explicit Domain(Fixture& value, Channel& channel) noexcept : fixture(value), pulse(channel)
        {
            assert(value_hook.prepare(1U) == EEndpointMutationError::NONE);
        }
    };

    auto install(SimulationBuilder& builder, SimulationSystemView view) noexcept
        -> lux::cxx::expected<void, SimulationSystemBuildFailure>
    {
        auto channel = builder.createHookChannel<EntityTargetedRoute<ecs::Entity>, installed_consumer::CollisionEvent>(
            view.instanceId(), PulseEvent, {1U, 2U}, [](const installed_consumer::CollisionEvent& value) noexcept {
                return installed_consumer::CollisionEvent{value.body, value.impulse};
            });
        if (!channel)
            return lux::cxx::unexpected(channel.error());
        auto domain = builder.emplaceSystem<Domain>(view.instanceId(), *builder.registry().ctx().get<Fixture*>(), **channel);
        if (!domain)
            return lux::cxx::unexpected(domain.error());
        auto writer = builder.bindHookChannelProducer(view.instanceId(), PrimarySimulationTask, **channel);
        if (!writer)
            return lux::cxx::unexpected(writer.error());
        (*domain)->pulse_writer = *writer;
        auto result = builder.publishScriptAbility(view.instanceId(),
            lux::script::bindScriptAbility<installed_consumer::InventoryAbility>(**domain));
        if (!result) return result;
        result = builder.publishScriptHook(view.instanceId(), (*domain)->hook_bridge.descriptor());
        if (!result)
            return result;
        result = builder.publishScriptEvent(view.instanceId(), (*domain)->event_bridge.descriptor());
        if (!result)
            return result;
        result = builder.addSystemTask<Domain>(view.instanceId(), [](Domain& value) noexcept {
            auto writer = value.pulse_writer.begin();
            return writer.record(value.fixture.entity, installed_consumer::CollisionEvent{17, 4.5F});
        });
        if (!result)
            return result;
        result = builder.addSystemHookTask<Domain>(view.instanceId(), ValueHook,
            [](Domain& value, const HookInvocation& invocation) noexcept {
                static_cast<void>(value.value_hook.dispatch(invocation, 2.5F));
            });
        if (!result)
            return result;
        return builder.addSystemHookTask<Domain>(view.instanceId(), EventHook,
            [](Domain&, const HookInvocation&) noexcept {});
    }

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


}
int main()
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    const auto& contract = generated::HookBehavior;
    Fixture fixture;
    auto description = materializeCppStaticScript(contract);
    assert(description);
    auto artifact = lux::script::ScriptArtifact::create(std::move(*description), {});
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
    assert(simulation_builder.addChannelProducer({SystemId, PulseEvent, SystemId, PrimarySimulationTask}));
    assert(simulation_builder.addExecutionDependency(SimulationExecutionPoint::task(SystemId),
        SimulationExecutionPoint::hook(SystemId, ValueHook)));
    assert(simulation_builder.addExecutionDependency(SimulationExecutionPoint::hook(SystemId, ValueHook),
        SimulationExecutionPoint::hook(SystemId, EventHook)));
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
    registry.ctx().emplace<Fixture*>(&fixture);
    SimulationSystemRegistry registrations;
    assert(registrations.add({lux::system::systemTypeId(Domain::Description.type.canonical_name),
        lux::cxx::typeToken<Domain>(), &Domain::Description, Domain::Access.spec(), {}, &install}));
    auto composed = Simulation::create(registry, simulation_owner, registrations);
    assert(composed);

    const std::array pools{CppStaticScriptPoolDescription{
        std::addressof(contract), 1U, 0U, 0U, alignof(std::max_align_t), 3U
    }};
    auto backend_result = CppStaticScriptBackend::create(pools);
    assert(backend_result);
    auto backend = std::move(*backend_result);
    const std::array backends{backend.descriptor()};
    auto created = ScriptSystem::create(
        *simulation_owner,
        *decoded_script,
        registry,
        composed->clock(),
        ScriptRuntimeLimits{2U, 1U, 2U, 2U, 2U, 2U, 64U, 2U, 2U, 2U, 2U, 2U},
        {&fixture, &resolveAsset},
        {&fixture, &resolveWorld},
        composed->scriptApiCapabilities(),
        backends,
        composed->scriptHookEndpoints(),
        composed->scriptEventEndpoints());
    assert(created);
    auto script_system = std::move(*created);
    assert(script_system.prepare());
    assert(script_system.activeInstanceCount() == 1U);

    auto connection = composed->bindHookCallbacks({&script_system,
        [](void* context, const SimulationClockSnapshot&, bool stable) noexcept {
            auto& runtime = *static_cast<ScriptSystem*>(context);
            if (stable)
                runtime.beginStableAdmission();
            return static_cast<bool>(runtime.processLifecycle());
        },
        [](void* context, const SimulationClockSnapshot&, bool stable) noexcept {
            return !stable || static_cast<bool>(static_cast<ScriptSystem*>(context)->executeStablePoint());
        }, nullptr});
    assert(connection);

    auto executor = lux::task::TaskExecutor::create({4U, 8U});
    assert(executor && composed->execute(*executor, SimulationDuration{1}));
    assert(installed_consumer::observed_value == 2.5F);
    assert(installed_consumer::observed_event.body == 17);
    assert(installed_consumer::observed_event.impulse == 4.5F);
    assert(script_system.shutdown());
    return 0;
}
