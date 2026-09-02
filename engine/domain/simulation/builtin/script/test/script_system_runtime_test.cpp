#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    inline constexpr lux::system::SystemInstanceId kSystem{41U};
    inline constexpr HookPointId kHook{42U};
    inline constexpr EventPointId kBroadcast{43U};
    inline constexpr EventPointId kTargeted{44U};
    inline constexpr HookPointId kSecondaryHook{45U};
    inline constexpr lux::script::ScriptSymbolId kHookSymbol{101U};
    inline constexpr lux::script::ScriptSymbolId kBroadcastSymbol{102U};
    inline constexpr lux::script::ScriptSymbolId kTargetedSymbol{103U};
    inline constexpr lux::script::ScriptSymbolId kSecondaryHookSymbol{104U};

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
            ),
            makeHookPointSpec<void(const SimulationStepInfo&)>(
                kSecondaryHook,
                "secondary"
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
        const SimulationSystemDescription system{
            .type = {.canonical_name = "lux.test.runtime", .version = 1U},
            .hooks = hooks,
            .events = events};
        SimulationDescriptionBuilder builder;
        assert(builder.addSystem(kSystem, "runtime", system));
        auto built = std::move(builder).build();
        assert(built);
        return std::move(*built);
    }

    [[nodiscard]] lux::script::ScriptArtifact makeArtifact()
    {
        const auto argument = lux::rdesc::makeScriptValueType<
            SimulationStepInfo>(lux::semantic::EValuePass::CONST_REF);
        lux::rdesc::Script description;
        description.module_name = "lux.test.runtime.script";
        description.exports = {
            {"on_tick", kHookSymbol, {argument}, {}},
            {"on_broadcast", kBroadcastSymbol, {argument}, {}},
            {"on_targeted", kTargetedSymbol, {argument}, {}},
            {"on_secondary", kSecondaryHookSymbol, {argument}, {}}};
        description.body = lux::rdesc::CppStaticScript{"fixture"};
        auto artifact = lux::script::ScriptArtifact::create(std::move(description), {});
        assert(artifact);
        return std::move(*artifact);
    }

    struct BackendState;

    struct BackendInstance final
    {
        BackendState* owner{};
        ScriptBehavior* behavior{};
        bool entity_scope{};
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
        std::size_t destroys{};
        std::size_t prepares{};
        std::size_t releases{};
        std::size_t hook_calls{};
        std::size_t secondary_hook_calls{};
        std::size_t broadcast_calls{};
        std::size_t targeted_calls{};
        std::size_t entity_calls{};
        bool fail_entity{};
        lux::script::ScriptSymbolId fail_symbol{};
        ScriptSystem* system{};
        bool request_shutdown{};
        std::optional<EScriptSystemError> shutdown_error;
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
        else if (call.symbol == kSecondaryHookSymbol)
            ++call.owner->secondary_hook_calls;
        else if (call.symbol == kBroadcastSymbol)
            ++call.owner->broadcast_calls;
        else if (call.symbol == kTargetedSymbol)
            ++call.owner->targeted_calls;
        else
            return 7;
        if (call.instance->entity_scope == call.owner->fail_entity &&
            call.symbol == call.owner->fail_symbol)
        {
            return 9;
        }
        if (call.owner->request_shutdown && call.symbol == kHookSymbol &&
            !call.instance->entity_scope)
        {
            call.owner->request_shutdown = false;
            const auto stopped = call.owner->system->shutdown();
            if (!stopped)
                call.owner->shutdown_error = stopped.error();
        }
        return 0;
    }

    EScriptBackendResult createInstance(
        void* context,
        const ScriptInstanceCreateContext& create,
        const lux::script::ScriptArtifact&,
        ScriptBackendInstance& output
    ) noexcept
    {
        auto& state = *static_cast<BackendState*>(context);
        auto instance = new (std::nothrow) BackendInstance{
            &state,
            create.behavior,
            std::holds_alternative<EntityScriptScope>(create.scope)};
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
        lux::script::ScriptArtifact artifact{makeArtifact()};
        ecs::Entity entity{ecs::NullEntity};
        std::size_t leases{};
        std::size_t releases{};
    };

    bool resolveAsset(
        void* context,
        const lux::asset::AssetId& id,
        ResolvedScriptArtifact& output
    ) noexcept
    {
        auto& fixture = *static_cast<Fixture*>(context);
        if (id != fixture.asset_id)
            return false;
        ++fixture.leases;
        output = {
            &fixture.artifact,
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

    struct ControlledHookEndpoint final
    {
        HookPointId id;
        bool fail_connect{};
        bool busy_disconnect{};
        void* lane_context{};
        ScriptHookLane lane{};
        std::size_t connect_count{};
        std::size_t disconnect_count{};

        [[nodiscard]] ScriptHookEndpointDescriptor descriptor() noexcept
        {
            return {
                kSystem,
                id,
                lux::simulation::detail::EndpointSignatureStorage<
                    void(const SimulationStepInfo&)>::view(),
                this,
                &connect,
                &disconnect
            };
        }

        static EndpointConnectResult connect(
            void* context,
            void* lane_context,
            ScriptHookLane lane
        ) noexcept
        {
            auto& self = *static_cast<ControlledHookEndpoint*>(context);
            ++self.connect_count;
            if (self.fail_connect)
                return {{}, EEndpointMutationError::CAPACITY_EXCEEDED};
            self.lane_context = lane_context;
            self.lane = lane;
            return {{1U, 1U}, EEndpointMutationError::NONE};
        }

        static EEndpointMutationError disconnect(
            void* context,
            EndpointConnectionToken token
        ) noexcept
        {
            auto& self = *static_cast<ControlledHookEndpoint*>(context);
            ++self.disconnect_count;
            if (self.busy_disconnect)
                return EEndpointMutationError::DISPATCH_ACTIVE;
            if (!token.valid() || self.lane_context == nullptr)
                return EEndpointMutationError::INVALID_TOKEN;
            self.lane_context = nullptr;
            self.lane = nullptr;
            return EEndpointMutationError::NONE;
        }
    };

    void testPrepareRollbackBusy()
    {
        auto simulation = makeSimulation();
        Fixture fixture;
        ecs::Registry registry;

        ScriptSystemDescriptionBuilder authored;
        assert(authored.addMount({
            ScriptMountId{7U},
            fixture.asset_id,
            SimulationScriptMount{},
            true,
            {
                {kHookSymbol, HookScriptTarget{kSystem, kHook}},
                {kSecondaryHookSymbol, HookScriptTarget{kSystem, kSecondaryHook}}
            }
        }));
        auto description = std::move(authored).build(simulation);
        assert(description);

        BackendState backend_state;
        const std::array backends{ScriptBackendDescriptor{
            lux::rdesc::Script::Kind::CPP_STATIC,
            &backend_state,
            &createInstance,
            &prepareMethod,
            &releaseMethod,
            &destroyInstance
        }};
        ControlledHookEndpoint first{kHook, false, true};
        ControlledHookEndpoint second{kSecondaryHook, true, false};
        const std::array endpoints{first.descriptor(), second.descriptor()};
        SimulationClock clock;
        auto created = ScriptSystem::create(
            simulation,
            *description,
            registry,
            clock,
            ScriptRuntimeLimits{2U, 1U, 4U, 4U, 4U, 4U, 64U, 4U},
            {&fixture, &resolveAsset},
            {},
            {},
            backends,
            endpoints,
            {}
        );
        assert(created);
        auto system = std::move(*created);

        const auto prepared = system.prepare();
        assert(!prepared);
        assert(prepared.error() == EScriptSystemError::ENDPOINT_BUSY);
        assert(first.lane_context != nullptr);
        assert(backend_state.creates == 1U);
        assert(backend_state.prepares == 2U);
        assert(backend_state.releases == 0U);
        assert(backend_state.destroys == 0U);
        assert(fixture.leases == 1U && fixture.releases == 0U);

        const auto repeated_prepare = system.prepare();
        assert(!repeated_prepare);
        assert(repeated_prepare.error() == EScriptSystemError::ENDPOINT_BUSY);
        const auto flushed = system.executeStablePoint();
        assert(!flushed);
        assert(flushed.error() == EScriptSystemError::ENDPOINT_BUSY);

        first.busy_disconnect = false;
        assert(system.shutdown());
        assert(first.lane_context == nullptr);
        assert(backend_state.releases == 2U);
        assert(backend_state.destroys == 1U);
        assert(fixture.leases == 1U && fixture.releases == 1U);
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
         {kTargetedSymbol, EventScriptTarget{kSystem, kTargeted}},
         {kSecondaryHookSymbol,
          HookScriptTarget{kSystem, kSecondaryHook}}}}));
    auto description = std::move(authored).build(simulation);
    assert(description);

    HookPoint<void(const SimulationStepInfo&)> hook;
    HookPoint<void(const SimulationStepInfo&)> secondary_hook;
    EventPoint<SimulationBroadcastRoute, SimulationStepInfo> broadcast;
    EventPoint<EntityTargetedRoute<ecs::Entity>, SimulationStepInfo> targeted;
    assert(hook.prepare(1U) == EEndpointMutationError::NONE);
    assert(secondary_hook.prepare(1U) == EEndpointMutationError::NONE);
    assert(broadcast.prepare(1U, 4U, 2U) == EEndpointMutationError::NONE);
    assert(targeted.prepare(1U, 4U, 2U) == EEndpointMutationError::NONE);

    ScriptHookEndpoint<void(const SimulationStepInfo&)> hook_bridge{
        kSystem,
        kHook,
        hook};
    ScriptHookEndpoint<void(const SimulationStepInfo&)> secondary_hook_bridge{
        kSystem,
        kSecondaryHook,
        secondary_hook};
    ScriptEventEndpoint<SimulationBroadcastRoute, SimulationStepInfo>
        broadcast_bridge{kSystem, kBroadcast, broadcast};
    ScriptEventEndpoint<
        EntityTargetedRoute<ecs::Entity>,
        SimulationStepInfo> targeted_bridge{kSystem, kTargeted, targeted};
    const std::array hook_endpoints{
        hook_bridge.descriptor(),
        secondary_hook_bridge.descriptor()};
    const std::array event_endpoints{
        broadcast_bridge.descriptor(),
        targeted_bridge.descriptor()};

    BackendState backend_state;
    const std::array backends{ScriptBackendDescriptor{
        lux::rdesc::Script::Kind::CPP_STATIC,
        &backend_state,
        &createInstance,
        &prepareMethod,
        &releaseMethod,
        &destroyInstance}};
    SimulationClock clock;
    auto created = ScriptSystem::create(
        simulation,
        *description,
        registry,
        clock,
        ScriptRuntimeLimits{8U, 2U, 8U, 8U, 8U, 8U, 64U, 8U},
        {&fixture, &resolveAsset},
        {&fixture, &resolveWorld},
        {},
        backends,
        hook_endpoints,
        event_endpoints
    );
    assert(created);
    auto system = std::move(*created);
    backend_state.system = &system;
    assert(system.prepare());
    assert(system.activeInstanceCount() == 2U);
    assert(hook.handlerCount() == 1U);
    assert(broadcast.handlerCount() == 1U);
    assert(targeted.handlerCount() == 1U);
    assert(backend_state.creates == 2U);
    assert(backend_state.prepares == 6U);

    const SimulationStepInfo step{1.0F / 60.0F, 12U};
    assert(hook.dispatch(step) == 1U);
    assert(backend_state.hook_calls == 2U);
    assert(backend_state.entity_calls == 1U);
    assert(secondary_hook.dispatch(step) == 1U);
    assert(backend_state.secondary_hook_calls == 1U);

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

    const auto destroyed_entity = fixture.entity;
    registry.destroy(destroyed_entity);
    const auto pending_world = system.executeStablePoint();
    assert(!pending_world);
    assert(pending_world.error() == EScriptSystemError::WORLD_OBJECT_NOT_RESOLVED);
    assert(system.activeInstanceCount() == 1U);

    fixture.entity = registry.create();
    assert(system.executeStablePoint());
    assert(system.activeInstanceCount() == 2U);
    assert(backend_state.creates == 3U);
    assert(backend_state.prepares == 10U);

    {
        auto writer = targeted.begin(0U);
        assert(writer.record(destroyed_entity, step));
        assert(writer.record(fixture.entity, step));
    }
    assert(targeted.drain() == 2U);
    assert(backend_state.targeted_calls == 2U);

    const auto releases_before_fault = backend_state.releases;
    backend_state.fail_entity = true;
    backend_state.fail_symbol = kHookSymbol;
    assert(hook.dispatch(step) == 1U);
    assert(system.activeInstanceCount() == 1U);
    assert(system.failures().size() == 1U);
    assert(system.failures().front().status == 9);
    assert(system.executeStablePoint());
    assert(backend_state.destroys == 2U);
    assert(backend_state.releases == 8U);
    assert(backend_state.releases - releases_before_fault == 4U);
    assert(targeted.targetBucketCount() == 0U);

    backend_state.request_shutdown = true;
    assert(hook.dispatch(step) == 1U);
    assert(backend_state.shutdown_error == EScriptSystemError::ENDPOINT_BUSY);
    assert(system.activeInstanceCount() == 1U);
    const auto stopping_stable_point = system.executeStablePoint();
    assert(!stopping_stable_point && stopping_stable_point.error() == EScriptSystemError::ENDPOINT_BUSY);

    assert(system.shutdown());
    assert(backend_state.destroys == 3U);
    assert(backend_state.releases == 10U);
    assert(fixture.leases == 3U && fixture.releases == 3U);
    testPrepareRollbackBusy();
    return 0;
}
