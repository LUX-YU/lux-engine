#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>
#include <lux/engine/simulation/script/ScriptSystem.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    inline constexpr SystemInstanceId kSystem{41U};
    inline constexpr HookPointId kHook{42U};
    inline constexpr EventPointId kBroadcast{43U};
    inline constexpr EventPointId kTargeted{44U};
    inline constexpr lux::script::ScriptSymbolId kHookSymbol{101U};
    inline constexpr lux::script::ScriptSymbolId kBroadcastSymbol{102U};
    inline constexpr lux::script::ScriptSymbolId kTargetedSymbol{103U};

    [[nodiscard]] lux::asset::AssetId assetId(std::uint8_t seed)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = seed;
        return lux::asset::AssetId{bytes};
    }

    [[nodiscard]] lux::world::WorldObjectId objectId(std::uint8_t seed)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = seed;
        return lux::world::WorldObjectId{uuids::uuid{bytes}};
    }

    [[nodiscard]] SimulationDescription makeSimulation()
    {
        constexpr std::array hooks{
            makeHookPointSpec<void(const SimulationStepInfo&)>(
                kHook,
                "tick"
            )};
        constexpr std::array events{
            makeEventPointSpec<SimulationStepInfo>(
                kBroadcast,
                "broadcast",
                kHook,
                EEventRoute::SIMULATION_BROADCAST,
                "lux.simulation.SimulationStepInfo",
                1U
            ),
            makeEventPointSpec<SimulationStepInfo>(
                kTargeted,
                "targeted",
                kHook,
                EEventRoute::ENTITY_TARGETED,
                "lux.simulation.SimulationStepInfo",
                1U
            )};
        const SystemDescription system{
            .canonical_name = "lux.test.runtime",
            .version = 1U,
            .hooks = hooks,
            .events = events};
        SimulationDescriptionBuilder builder;
        assert(builder.addSystem(kSystem, "runtime", system));
        auto built = std::move(builder).build();
        assert(built);
        return std::move(*built);
    }

    [[nodiscard]] lux::asset::ScriptAssetContent makeAsset()
    {
        const auto argument = lux::rdesc::makeScriptValueType<
            SimulationStepInfo>(lux::script::EScriptPassMode::CONST_REF);
        lux::asset::ScriptAssetContent asset;
        asset.description.module_name = "lux.test.runtime.script";
        asset.description.exports = {
            {"on_tick", kHookSymbol, {argument}, {}},
            {"on_broadcast", kBroadcastSymbol, {argument}, {}},
            {"on_targeted", kTargetedSymbol, {argument}, {}}};
        asset.description.body = lux::rdesc::CppStaticScript{"fixture"};
        assert(lux::rdesc::validScriptDescription(asset.description));
        return asset;
    }

    struct BackendState;

    struct BackendInstance final
    {
        BackendState* owner{};
        ScriptBehavior* behavior{};
    };

    struct PreparedCall final
    {
        BackendState* owner{};
        BackendInstance* instance{};
        lux::script::ScriptSymbolId symbol{};
    };

    struct BackendState final
    {
        std::size_t creates{};
        std::size_t starts{};
        std::size_t stops{};
        std::size_t destroys{};
        std::size_t prepares{};
        std::size_t releases{};
        std::size_t hook_calls{};
        std::size_t broadcast_calls{};
        std::size_t targeted_calls{};
        std::size_t entity_calls{};
        std::vector<EBehaviorStopReason> stop_reasons;
    };

    int invoke(lux_script_call_frame* frame)
    {
        assert(frame && frame->arg_count == 1U && frame->args);
        auto& call = *static_cast<PreparedCall*>(frame->user_context);
        assert(call.instance && call.instance->behavior);
        if (call.instance->behavior->hasSelf())
            ++call.owner->entity_calls;
        if (call.symbol == kHookSymbol)
            ++call.owner->hook_calls;
        else if (call.symbol == kBroadcastSymbol)
            ++call.owner->broadcast_calls;
        else if (call.symbol == kTargetedSymbol)
            ++call.owner->targeted_calls;
        else
            return 7;
        return 0;
    }

    EScriptBackendResult createInstance(
        void* context,
        const ScriptInstanceCreateContext& create,
        const lux::asset::ScriptAssetContent&,
        ScriptBackendInstance& output
    ) noexcept
    {
        auto& state = *static_cast<BackendState*>(context);
        auto instance = new (std::nothrow) BackendInstance{
            &state,
            create.behavior};
        if (!instance)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        ++state.creates;
        output.value = instance;
        return EScriptBackendResult::SUCCESS;
    }

    EScriptBackendResult prepareMethod(
        void*,
        ScriptBackendInstance instance,
        const lux::rdesc::ScriptFunction& function,
        lux::script::BoundScriptCall& output
    ) noexcept
    {
        auto* backend = static_cast<BackendInstance*>(instance.value);
        auto* call = new (std::nothrow) PreparedCall{
            backend->owner,
            backend,
            function.symbol_id};
        if (!call)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        ++backend->owner->prepares;
        output = {&invoke, call};
        return EScriptBackendResult::SUCCESS;
    }

    EScriptBackendResult startInstance(
        void*,
        ScriptBackendInstance instance
    ) noexcept
    {
        auto* backend = static_cast<BackendInstance*>(instance.value);
        ++backend->owner->starts;
        return EScriptBackendResult::SUCCESS;
    }

    void stopInstance(
        void*,
        ScriptBackendInstance instance,
        EBehaviorStopReason reason
    ) noexcept
    {
        auto* backend = static_cast<BackendInstance*>(instance.value);
        ++backend->owner->stops;
        backend->owner->stop_reasons.push_back(reason);
    }

    void releaseMethod(
        void*,
        ScriptBackendInstance instance,
        lux::script::BoundScriptCall call
    ) noexcept
    {
        auto* backend = static_cast<BackendInstance*>(instance.value);
        ++backend->owner->releases;
        delete static_cast<PreparedCall*>(call.context);
    }

    void destroyInstance(void*, ScriptBackendInstance instance) noexcept
    {
        auto* backend = static_cast<BackendInstance*>(instance.value);
        ++backend->owner->destroys;
        delete backend;
    }

    struct Fixture final
    {
        lux::asset::AssetId asset_id{assetId(9U)};
        lux::world::WorldObjectId object{objectId(8U)};
        lux::asset::ScriptAssetContent asset{makeAsset()};
        ecs::Entity entity{ecs::NullEntity};
        std::size_t leases{};
        std::size_t releases{};
    };

    bool resolveAsset(
        void* context,
        const lux::asset::AssetId& id,
        ResolvedScriptAsset& output
    ) noexcept
    {
        auto& fixture = *static_cast<Fixture*>(context);
        if (id != fixture.asset_id)
            return false;
        ++fixture.leases;
        output = {
            &fixture.asset,
            &fixture,
            [](void* value) noexcept
            {
                ++static_cast<Fixture*>(value)->releases;
            }};
        return true;
    }

    bool resolveWorld(
        void* context,
        const lux::world::WorldObjectId& object,
        ecs::Entity& output
    ) noexcept
    {
        auto& fixture = *static_cast<Fixture*>(context);
        if (object != fixture.object)
            return false;
        output = fixture.entity;
        return true;
    }
}

int main()
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    auto simulation = makeSimulation();
    Fixture fixture;
    ecs::Registry registry;
    fixture.entity = registry.create();

    ScriptSystemDescriptionBuilder authored;
    assert(authored.addMount({
        ScriptMountId{1U},
        fixture.asset_id,
        SimulationScriptMount{},
        true,
        {{kHookSymbol, HookScriptTarget{kSystem, kHook}},
         {kBroadcastSymbol, EventScriptTarget{kSystem, kBroadcast}}}}));
    assert(authored.addMount({
        ScriptMountId{2U},
        fixture.asset_id,
        EntityScriptMount{fixture.object},
        true,
        {{kHookSymbol, HookScriptTarget{kSystem, kHook}},
         {kBroadcastSymbol, EventScriptTarget{kSystem, kBroadcast}},
         {kTargetedSymbol, EventScriptTarget{kSystem, kTargeted}}}}));
    auto description = std::move(authored).build(simulation);
    assert(description);

    HookPoint<void(const SimulationStepInfo&)> hook;
    EventPoint<SimulationBroadcastRoute, SimulationStepInfo> broadcast;
    EventPoint<EntityTargetedRoute<ecs::Entity>, SimulationStepInfo> targeted;
    assert(hook.prepare(2U, 4U) == EEndpointMutationError::NONE);
    assert(broadcast.prepare(1U, 4U, 2U, 4U) ==
        EEndpointMutationError::NONE);
    assert(targeted.prepare(1U, 4U, 2U, 4U) ==
        EEndpointMutationError::NONE);

    ScriptHookEndpoint<void(const SimulationStepInfo&)> hook_bridge{
        kSystem,
        kHook,
        hook};
    ScriptEventEndpoint<SimulationBroadcastRoute, SimulationStepInfo>
        broadcast_bridge{kSystem, kBroadcast, broadcast};
    ScriptEventEndpoint<
        EntityTargetedRoute<ecs::Entity>,
        SimulationStepInfo> targeted_bridge{kSystem, kTargeted, targeted};
    const std::array hook_endpoints{hook_bridge.descriptor()};
    const std::array event_endpoints{
        broadcast_bridge.descriptor(),
        targeted_bridge.descriptor()};

    BackendState backend_state;
    const std::array backends{ScriptBackendDescriptor{
        lux::rdesc::Script::Kind::CPP_STATIC,
        &backend_state,
        &createInstance,
        &prepareMethod,
        &startInstance,
        &stopInstance,
        &releaseMethod,
        &destroyInstance}};
    const ScriptSystemCapacities capacities{
        4U,
        8U,
        2U,
        2U,
        8U,
        8U,
        8U};
    auto created = ScriptSystem::create(
        simulation,
        *description,
        registry,
        capacities,
        {&fixture, &resolveAsset},
        {&fixture, &resolveWorld},
        backends,
        hook_endpoints,
        event_endpoints
    );
    assert(created);
    auto system = std::move(*created);
    assert(system.prepare());
    assert(system.activeInstanceCount() == 2U);
    assert(backend_state.creates == 2U);
    assert(backend_state.starts == 2U);
    assert(backend_state.prepares == 5U);

    const SimulationStepInfo step{1.0F / 60.0F, 12U};
    assert(hook.dispatch(step) == 1U);
    assert(backend_state.hook_calls == 2U);
    assert(backend_state.entity_calls == 1U);

    {
        auto writer = broadcast.begin(0U);
        assert(writer.record(step));
    }
    assert(broadcast.drain() == 1U);
    assert(backend_state.broadcast_calls == 2U);

    const auto other = registry.create();
    {
        auto writer = targeted.begin(0U);
        assert(writer.record(other, step));
        assert(writer.record(fixture.entity, step));
    }
    assert(targeted.drain() == 2U);
    assert(backend_state.targeted_calls == 1U);

    registry.remove<lux::simulation::script::detail::ScriptAttachment>(
        fixture.entity
    );
    assert(system.flushMutations());
    assert(system.activeInstanceCount() == 1U);
    assert(backend_state.stops == 1U);
    assert(backend_state.stop_reasons.back() ==
        EBehaviorStopReason::ENTITY_DESTROYED);

    assert(system.shutdown());
    assert(backend_state.stops == 2U);
    assert(backend_state.destroys == 2U);
    assert(backend_state.releases == 5U);
    assert(fixture.leases == 2U && fixture.releases == 2U);
    return 0;
}
