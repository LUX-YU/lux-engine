#include "../../../system/test/HookInvocationTestAccess.hpp"
#include "../../../scripting/core/test/ScriptEndpointTestAccess.hpp"
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/script/ScriptBindings.hpp>
#include <lux/engine/simulation/script/ScriptInstances.hpp>

#include <array>
#include <cassert>
#include <cstdio>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    using namespace lux::simulation::script::detail;
    constexpr lux::system::SystemInstanceId kSystem{0x8301U};
    constexpr HookPointId kHook{0x8302U};
    constexpr EventPointId kEvent{0x8303U};

    struct Dispatch final
    {
        ScriptBindings* bindings{};
        ecs::Entity entity{ecs::NullEntity};
        std::size_t calls{};
        bool withdraw{};
        static void hook(void* context, std::uint32_t slot, lux_script_call_frame& frame) noexcept
        {
            static_cast<Dispatch*>(context)->bindings->visitHook(slot, [&](auto reference) noexcept {
                invoke(context, reference, frame, true);
            });
        }
        static void event(void* context, std::uint32_t slot, ecs::Entity entity,
            lux_script_call_frame& frame) noexcept
        {
            static_cast<Dispatch*>(context)->bindings->visitEvent(slot, entity, [&](auto reference) noexcept {
                invoke(context, reference, frame, false);
            });
        }
        static void invoke(void* context, ScriptMethodReference reference, lux_script_call_frame&, bool) noexcept
        {
            auto& self = *static_cast<Dispatch*>(context);
            ++self.calls;
            assert(reference.mount_slot == 0U);
            if (!self.withdraw)
                return;
            self.withdraw = false;
            self.bindings->withdraw(0U);
            self.bindings->withdraw(0U);
            const auto publication = self.bindings->publish(0U, reference.instance, self.entity);
            assert(!publication && publication.error() == EScriptSystemError::ENDPOINT_BUSY);
            const auto disconnected = self.bindings->disconnect();
            assert(!disconnected && disconnected.error() == EScriptSystemError::ENDPOINT_BUSY);
            // Busy Hook disconnect retained its actual token; reconnect must not duplicate it.
            assert(self.bindings->connect());
        }
    };
    // Owner-level admission test; lifecycle_test exercises actual prepared calls and foreign reentry.
    void testInvocationAuthority(const ScriptRuntimeCapacityPlan& capacity, const ScriptBindings& bindings,
        std::span<const ScriptRuntimeMount> inputs, ecs::Registry& registry)
    {
        ScriptInstances instances;
        assert(instances.prepare(capacity, 1U, registry, {}));
        auto batch = instances.reserveBatch(inputs, bindings, true);
        assert(batch);
        instances.commitBatch(std::move(*batch), bindings);
        auto construction = instances.beginConstruction(0U);
        assert(construction && *construction);
        assert((*construction)->allocateIdentity());
        (*construction)->commit();
        instances.activate(0U);
        const auto id = instances.view(0U).instance;
        const auto layout = bindings.layout(0U);
        const ScriptMethodReference reference{0U, static_cast<std::uint32_t>(layout.method_first), id};
        assert(!instances.invokeAccess({1U, reference.method_slot, id}));
        assert(!instances.invokeAccess({0U, reference.method_slot, {}}));
        assert(!instances.invokeAccess({0U, reference.method_slot, {id.slot, id.generation + 1U}}));
        assert(!instances.invokeAccess({0U,
            static_cast<std::uint32_t>(layout.method_first + layout.method_count), id}));
        assert(instances.protectedCount() == 0U);
        const ScriptPreparedMethod* address{};
        {
            auto access = instances.invokeAccess(reference);
            assert(access && access.current() && instances.protectedCount() == 1U);
            address = &access.method();
            assert(address->symbol == lux::script::ScriptSymbolId{1U});
            auto moved = std::move(access);
            assert(!access && moved.current() && instances.protectedCount() == 1U);
            assert(instances.revoke(0U) == id);
            assert(!moved.current() && moved.sameIncarnation());
            assert(!instances.invokeAccess(reference) && !instances.resumeAccess(id));
            assert(&moved.method() == address && instances.protectedCount() == 1U);
        }
        assert(instances.protectedCount() == 0U);
        auto retired = instances.claimRetirement(0U, EScriptEndPlayReason::OBJECT_UNMATERIALIZED,
            EScriptMountState::INACTIVE);
        assert(instances.endPlay(retired));
        instances.finishRetirement(retired);
        assert(instances.query(inputs[0].id)->reclaimed);
        std::array<ScriptMountStatus, 1U> changes;
        static_cast<void>(instances.collect(changes));
        auto replacement = inputs[0];
        replacement.scope = EntityScriptScope{registry.create()};
        auto next_batch = instances.reserveBatch(std::span{&replacement, 1U}, bindings, false);
        assert(next_batch);
        instances.commitBatch(std::move(*next_batch), bindings);
        auto next = instances.beginConstruction(0U);
        assert(next && *next && (*next)->allocateIdentity());
        (*next)->commit();
        instances.activate(0U);
        const auto next_id = instances.view(0U).instance;
        assert(next_id.slot == id.slot && next_id.generation != id.generation);
        assert(!instances.invokeAccess(reference) && !instances.resumeAccess(id));
        {
            auto access = instances.invokeAccess({0U, reference.method_slot, next_id});
            assert(access && access.current() && &access.method() == address);
        }
        static_cast<void>(instances.revoke(0U));
        auto final = instances.claimRetirement(0U, EScriptEndPlayReason::OBJECT_UNMATERIALIZED,
            EScriptMountState::INACTIVE);
        instances.finishRetirement(final);
        assert(instances.protectedCount() == 0U && instances.activeCount() == 0U);
        std::puts("INVOCATION_AUTHORITY_OK,bounds=2,identity=2,move_pin=1,retired_denied=1,reused_denied=1,stable=1");
    }
}

int main()
{
    constexpr std::array hooks{makeHookPointSpec<void()>(kHook, "tick")};
    constexpr std::array events{makeEventPointSpec<std::int32_t>(kEvent, "target", kHook,
        EEventRoute::ENTITY_TARGETED, "lux.i32", 1U)};
    const SimulationSystemDescription description{
        .type = {.canonical_name = "lux.test.bindings-owner", .version = 1U}, .hooks = hooks, .events = events
    };
    SimulationDescriptionBuilder builder;
    assert(builder.addSystem(kSystem, "bindings", description));
    auto simulation = std::move(builder).build();
    assert(simulation);
    HookPoint<void()> hook;
    HookChannel<EntityTargetedRoute<ecs::Entity>, std::int32_t> event;
    assert(hook.prepare(1U) == EEndpointMutationError::NONE);
    assert(event.prepare({1U, 2U}) == EEndpointMutationError::NONE);
    ScriptHookEndpoint<void()> hook_bridge{kSystem, kHook, hook};
    ScriptEventEndpoint<EntityTargetedRoute<ecs::Entity>, std::int32_t> event_bridge{kSystem, kEvent, event};
    const std::array hook_endpoints{hook_bridge.descriptor()};
    const std::array event_endpoints{event_bridge.descriptor()};
    ecs::Registry registry;
    const auto entity = registry.create();
    std::array<std::uint8_t, 16U> asset_bytes{};
    asset_bytes.front() = 1U;
    const std::array inputs{ScriptRuntimeMount{ScriptMountId{1U}, lux::asset::AssetId{asset_bytes},
        EntityScriptScope{entity}, {
            {lux::script::ScriptSymbolId{1U}, HookScriptTarget{kSystem, kHook}},
            {lux::script::ScriptSymbolId{2U}, EventScriptTarget{kSystem, kEvent}}
        }}};
    const auto capacity = planScriptRuntimeCapacity(inputs);
    assert(capacity);
    ScriptBindings bindings;
    Dispatch dispatch{&bindings, entity};
    assert(bindings.prepare(*simulation, *capacity, hook_endpoints, event_endpoints,
        {&dispatch, &Dispatch::hook, &Dispatch::event}, 64U));
    const std::array placements{ScriptMountPlacement{0U, false}};
    const auto bytes = bindings.backingBytes();
    const std::array rejected_inputs{inputs[0], inputs[0]};
    const std::array rejected_placements{placements[0], ScriptMountPlacement{1U, false}};
    const auto rejected_batch = bindings.reserveBatch(rejected_inputs, rejected_placements);
    assert(!rejected_batch && rejected_batch.error() == EScriptSystemError::INVALID_INPUT);
    assert(bindings.methodCount() == 0U && bindings.backingBytes() == bytes);
    {
        auto ticket = bindings.reserveBatch(inputs, placements);
        assert(ticket);
        assert(!bindings.reserveBatch(inputs, placements));
    }
    assert(bindings.methodCount() == 0U && bindings.backingBytes() == bytes);
    auto ticket = bindings.reserveBatch(inputs, placements);
    assert(ticket);
    bindings.commitBatch(std::move(*ticket));
    const auto method_count = bindings.methodCount();
    assert(method_count == 4U); // Two fixed lifecycle slots plus the two bound methods.
    assert(bindings.connect());
    assert(bindings.connect());
    const ScriptInstanceId instance{1U, 1U};
    const auto rejected = bindings.publish(0U, instance, ecs::NullEntity);
    assert(!rejected && rejected.error() == EScriptSystemError::SCOPE_MISMATCH);
    assert(lux::simulation::test::dispatchHookForTest(hook));
    assert(dispatch.calls == 0U);
    // The failed Event publication must have rolled back the preceding Hook registration.
    for (std::size_t iteration{}; iteration < 128U; ++iteration)
    {
        assert(bindings.publish(0U, instance, entity));
        assert(!bindings.publish(0U, instance, entity));
        dispatch.withdraw = true;
        const auto before = dispatch.calls;
        assert(lux::simulation::test::dispatchHookForTest(hook));
        assert(dispatch.calls == before + 1U);
        assert(lux::simulation::test::dispatchHookForTest(hook));
        assert(dispatch.calls == before + 1U);
        assert(bindings.methodCount() == method_count);
    }
    assert(bindings.publish(0U, instance, entity));
    {
        auto writer = event.begin(0U);
        assert(writer.record(entity, 42));
    }
    assert(lux::simulation::script::test::deliverEndpoint(event_bridge) == 1U);
    assert(dispatch.calls == 129U);
    bindings.withdraw(0U);
    assert(bindings.disconnect());
    testInvocationAuthority(*capacity, bindings, inputs, registry);
    assert(bindings.disconnect());
    assert(bindings.connect());
    assert(bindings.publish(0U, instance, entity));
    assert(lux::simulation::test::dispatchHookForTest(hook));
    assert(dispatch.calls == 130U);
    bindings.withdraw(0U);
    assert(bindings.disconnect());
    std::printf("BINDINGS_OK,rollback=1,aborted_ticket=1,rebuilds=128,calls=%zu\n", dispatch.calls);
}
