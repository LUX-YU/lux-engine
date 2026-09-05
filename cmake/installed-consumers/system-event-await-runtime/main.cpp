#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    inline constexpr lux::system::SystemInstanceId kSystem{0x6500U};
    inline constexpr HookPointId kDispatch{0x6501U};
    inline constexpr EventPointId kStart{0x6502U};
    inline constexpr EventPointId kReady{0x6503U};
    inline constexpr lux::script::ScriptSymbolId kSymbol{0x6504U};

    struct Producer final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr std::array Hooks{makeHookPointSpec<void()>(kDispatch, "dispatch", true, true)};
        inline static constexpr std::array Events{
            makeEventPointSpec<std::int32_t>(kStart, "start", kDispatch,
                EEventRoute::SIMULATION_BROADCAST, "lux.i32", 1U),
            makeEventPointSpec<std::int32_t>(kReady, "ready", kDispatch,
                EEventRoute::SIMULATION_BROADCAST, "lux.i32", 1U)
        };
        inline static constexpr SimulationSystemDescription Description{
            .type = {.canonical_name = "consumer.event-await", .version = 1U}, .hooks = Hooks, .events = Events};
        HookChannel<SimulationBroadcastRoute, std::int32_t> start;
        HookChannel<SimulationBroadcastRoute, std::int32_t> ready;
        ScriptEventEndpoint<SimulationBroadcastRoute, std::int32_t> start_bridge{kSystem, kStart, start};
        ScriptEventEndpoint<SimulationBroadcastRoute, std::int32_t> ready_bridge{kSystem, kReady, ready};
        unsigned steps{};
        Producer() noexcept
        {
            assert(start.prepare({1U, 1U}) == EEndpointMutationError::NONE);
            assert(ready.prepare({1U, 1U}) == EEndpointMutationError::NONE);
        }
    };

    auto install(SimulationBuilder& builder, SimulationSystemView view) noexcept
        -> lux::cxx::expected<void, SimulationSystemBuildFailure>
    {
        auto value = builder.emplaceSystem<Producer>(view.instanceId());
        if (!value)
            return lux::cxx::unexpected(value.error());
        auto result = builder.publishScriptEvent(view.instanceId(), (*value)->start_bridge.descriptor());
        if (!result)
            return result;
        result = builder.publishScriptEvent(view.instanceId(), (*value)->ready_bridge.descriptor());
        if (!result)
            return result;
        result = builder.addSystemTask<Producer>(view.instanceId(), [](Producer& producer) noexcept {
            const auto step = ++producer.steps;
            auto writer = step == 1U ? producer.start.begin(0U) : producer.ready.begin(0U);
            std::int32_t payload = step == 1U ? 1 : 42;
            const auto recorded = writer.record(payload);
            payload = -1;
            return recorded;
        });
        if (!result)
            return result;
        return builder.addSystemHookTask<Producer>(view.instanceId(), kDispatch,
            [](Producer&, const HookInvocation&) noexcept {});
    }

    struct BackendState final
    {
        std::int32_t resumed_value{};
        std::size_t resumes{};
    };

    struct Continuation final
    {
        BackendState* state{};
    };

    [[nodiscard]] lux::asset::AssetId assetId()
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = 0x65U;
        return lux::asset::AssetId{bytes};
    }

    void destroyContinuation(void* opaque) noexcept
    {
        delete static_cast<Continuation*>(opaque);
    }

    ScriptStepResult resume(
        void* opaque,
        ScriptStepContext&,
        const ScriptResumePacket& packet
    ) noexcept
    {
        auto& state = *static_cast<Continuation*>(opaque)->state;
        assert(packet.state == EScriptAwaitableState::READY);
        assert(packet.value != nullptr && packet.value->bytes.size() == sizeof(std::int32_t));
        std::memcpy(std::addressof(state.resumed_value), packet.value->bytes.data(), sizeof(state.resumed_value));
        ++state.resumes;
        return ScriptStepResult::completed();
    }

    ScriptStepResult invokeStep(
        void* context,
        lux_script_call_frame&,
        ScriptStepContext& step,
        ScriptBackendContinuation& continuation
    ) noexcept
    {
        auto waiting = step.event_waits.wait({kSystem, kReady, EEventRoute::SIMULATION_BROADCAST});
        if (!waiting)
            return ScriptStepResult::failed(1);
        auto* stored = new (std::nothrow) Continuation{static_cast<BackendState*>(context)};
        if (stored == nullptr)
        {
            step.awaitables.discard(*waiting);
            return ScriptStepResult::failed(2);
        }
        continuation = {stored, &resume, &destroyContinuation};
        return ScriptStepResult::suspended(*waiting);
    }

    int invokeSync(lux_script_call_frame*)
    {
        return 0;
    }

    EScriptBackendResult createInstance(
        void* context,
        const ScriptInstanceCreateContext&,
        const lux::script::ScriptArtifact&,
        ScriptBackendInstance& output
    ) noexcept
    {
        output.value = context;
        return EScriptBackendResult::SUCCESS;
    }

    EScriptBackendResult prepareMethod(
        void*,
        ScriptBackendInstance instance,
        const lux::rdesc::ScriptFunction&,
        ScriptBackendPreparedMethod& output
    ) noexcept
    {
        output = {
            instance.value,
            lux::script::BoundScriptCall{&invokeSync, instance.value},
            BoundScriptStepCall{instance.value, &invokeStep}
        };
        return EScriptBackendResult::SUCCESS;
    }

    void releaseMethod(void*, ScriptBackendInstance, ScriptBackendPreparedMethod) noexcept
    {
    }

    void destroyInstance(void*, ScriptBackendInstance) noexcept
    {
    }

    struct ArtifactSource final
    {
        lux::asset::AssetId id{assetId()};
        lux::script::ScriptArtifact artifact;

        static bool resolve(
            void* context,
            const lux::asset::AssetId& requested,
            ResolvedScriptArtifact& output
        ) noexcept
        {
            auto& self = *static_cast<ArtifactSource*>(context);
            if (requested != self.id)
                return false;
            output.artifact = std::addressof(self.artifact);
            return true;
        }
    };
}

int main()
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    SimulationDescriptionBuilder simulation_builder;
    assert(simulation_builder.addSystem(kSystem, "event-await", Producer::Description));
    assert(simulation_builder.addExecutionDependency(
        SimulationExecutionPoint::task(kSystem), SimulationExecutionPoint::hook(kSystem, kDispatch)));
    auto built = std::move(simulation_builder).build();
    assert(built);
    auto simulation = std::make_shared<SimulationDescription>(std::move(*built));
    ecs::Registry registry;
    SimulationSystemRegistry registrations;
    assert(registrations.add({lux::system::systemTypeId(Producer::Description.type.canonical_name),
        lux::cxx::typeToken<Producer>(), &Producer::Description, Producer::Access.spec(), {}, &install}));
    auto composed = Simulation::create(registry, simulation, registrations);
    assert(composed);
    const auto endpoints = composed->scriptEventEndpoints();
    const auto projected_event = projectScriptEventSource(
        simulation->findEvent(kSystem, kReady),
        endpoints[1],
        "Gameplay",
        "ready"
    );
    assert(projected_event && projected_event->system_id == kSystem.value &&
        projected_event->event_id == kReady.value && projected_event->payload.canonical_name == "lux.i32");

    lux::rdesc::Script description;
    description.module_name = "consumer.event-await.script";
    description.exports.push_back({
        "start",
        kSymbol,
        {lux::rdesc::makeScriptValueType<std::int32_t>(lux::semantic::EValuePass::CONST_REF)},
        {}
    });
    description.event_requirements.push_back(*projected_event);
    description.body = lux::rdesc::CppStaticScript{"consumer-event-await"};
    auto artifact = lux::script::ScriptArtifact::create(std::move(description), {});
    assert(artifact);
    ArtifactSource source{assetId(), std::move(*artifact)};

    ScriptSystemDescriptionBuilder script_builder;
    assert(script_builder.addMount({
        ScriptMountId{1U},
        source.id,
        SimulationScriptMount{},
        true,
        {{kSymbol, EventScriptTarget{kSystem, kStart}}}
    }));
    auto scripts = std::move(script_builder).build(*simulation);
    assert(scripts);

    BackendState backend_state;
    const std::array backends{ScriptBackendDescriptor{
        lux::rdesc::Script::Kind::CPP_STATIC,
        std::addressof(backend_state),
        &createInstance,
        &prepareMethod,
        &releaseMethod,
        &destroyInstance
    }};
    auto created = ScriptSystem::create(
        *simulation,
        *scripts,
        registry,
        composed->clock(),
        {8U, 1U, 2U, 2U, 2U, 2U, 64U, 2U, 2U, 2U, 2U, 2U},
        {std::addressof(source), &ArtifactSource::resolve},
        {},
        {},
        backends,
        {},
        endpoints
    );
    assert(created);
    auto system = std::move(*created);
    assert(system.prepare());

    struct Context { ScriptSystem& system; BackendState& backend; } context{system, backend_state};
    auto connection = composed->bindHookCallbacks({&context,
        [](void* opaque, const SimulationClockSnapshot&, bool) noexcept {
            auto& context = *static_cast<Context*>(opaque);
            context.system.beginStableAdmission();
            return static_cast<bool>(context.system.processLifecycle());
        },
        [](void* opaque, const SimulationClockSnapshot&, bool) noexcept {
            auto& context = *static_cast<Context*>(opaque);
            assert(context.backend.resumes == 0U);
            return static_cast<bool>(context.system.executeStablePoint());
        }, nullptr});
    assert(connection);
    auto executor = lux::task::TaskExecutor::create({4U, 8U});
    assert(executor && composed->execute(*executor, SimulationDuration{1}));
    assert(system.stats().active_event_waiters == 1U && backend_state.resumes == 0U);
    assert(composed->execute(*executor, SimulationDuration{1}));
    assert(backend_state.resumes == 1U && backend_state.resumed_value == 42);
    assert(system.shutdown());
    return 0;
}
