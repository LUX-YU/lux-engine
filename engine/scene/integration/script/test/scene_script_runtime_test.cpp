#include <lux/engine/scene/script/ScriptRuntimeAssembly.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/SceneBuilder.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/ScriptRuntimeSystem.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/scene/script/ScriptSystemDescriptionCodec.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>
#include "DelayAbility.ability.generated.hpp"
#include <lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using namespace lux;
    using namespace lux::scene;
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    using namespace lux::scene::script;

    inline constexpr system::SystemInstanceId kProbeSystem{0x7201U};
    inline constexpr system::SystemInstanceId kScriptRuntime{0x7202U};
    inline constexpr HookPointId kTickHook{0x7203U};
    inline constexpr lux::script::ScriptSymbolId kTickSymbol{0x7204U};
    inline constexpr system::SystemInstanceId kStableProbe{0x7205U};
    inline constexpr SimulationTaskId kPropagate{2U};
    using DelayAbility = lux::simulation::script::DelayAbility;
    using DelayAbilityTraits = lux::script::ScriptAbilityTraits<DelayAbility>;

    [[nodiscard]] asset::AssetId assetId(std::uint8_t value)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = value;
        return asset::AssetId{bytes};
    }

    template <class Type>
    [[nodiscard]] Type worldId(std::uint8_t value)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[15] = value;
        return Type{uuids::uuid(bytes)};
    }

    struct ProbeSystem final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr std::array Hooks{makeHookPointSpec<void()>(kTickHook, "tick", true, true)};
        inline static constexpr std::array Stages{
            SimulationTaskSpec{PrimarySimulationTask, "produce"}, SimulationTaskSpec{kPropagate, "propagate"}
        };
        inline static constexpr SimulationSystemDescription Description{
            .type = {.canonical_name = "lux.test.scene-script.probe", .version = 1U},
            .hooks = Hooks,
            .tasks = Stages
        };

        ProbeSystem() noexcept : endpoint(kProbeSystem, kTickHook, hook)
        {
            ready = hook.prepare(2U) == EEndpointMutationError::NONE;
        }

        void execute() noexcept
        {
        }

        HookPoint<void()> hook;
        ScriptHookEndpoint<void()> endpoint;
        bool ready{};
        bool tick_requested{true};
        std::size_t live_value{};
        std::size_t derived_value{};
    };

    ProbeSystem* active_probe{};

    [[nodiscard]] lux::cxx::expected<void, SimulationSystemBuildFailure> installProbe(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept
    {
        auto probe = builder.emplaceSystem<ProbeSystem>(description.instanceId());
        if (!probe)
            return lux::cxx::unexpected(probe.error());
        if (!(*probe)->ready)
        {
            return lux::cxx::unexpected(SimulationSystemBuildFailure{
                ESimulationSystemBuildError::CONSTRUCTION_FAILURE,
                description.instanceId()
            });
        }
        auto published = builder.publishScriptHook(description.instanceId(), (*probe)->endpoint.descriptor());
        if (!published)
            return published;
        active_probe = *probe;
        const auto task = builder.addSystemTask<ProbeSystem>(
            description.instanceId(),
            [](ProbeSystem& value) noexcept { value.execute(); }
        );
        if (!task)
            return task;
        const auto propagation = builder.addSystemTask<ProbeSystem>(description.instanceId(),
            [](ProbeSystem& value) noexcept { value.derived_value = value.live_value * 10U; }, kPropagate);
        if (!propagation)
            return propagation;
        return builder.addSystemHookTask<ProbeSystem>(description.instanceId(), kTickHook,
            [](ProbeSystem& value, const HookInvocation& invocation) noexcept {
                if (std::exchange(value.tick_requested, false))
                    static_cast<void>(value.hook.dispatch(invocation));
            });
    }

    [[nodiscard]] SimulationSystemRegistration probeRegistration() noexcept
    {
        return {
            .type = system::systemTypeId(ProbeSystem::Description.type.canonical_name),
            .cpp_type = cxx::typeToken<ProbeSystem>(),
            .description = &ProbeSystem::Description,
            .access = ProbeSystem::Access.spec(),
            .configuration = {},
            .install = &installProbe
        };
    }

    [[nodiscard]] SimulationDescription makeSimulationDescription(
        const ScriptSystemDescription& script_description,
        ScriptSystemCodecLimits codec_limits
    )
    {
        SimulationDescriptionBuilder builder;
        assert(builder.addSystem(kProbeSystem, "probe", ProbeSystem::Description));
        assert(builder.addExecutionDependency(SimulationExecutionPoint::task(kProbeSystem),
            SimulationExecutionPoint::hook(kProbeSystem, kTickHook)));
        assert(builder.addExecutionDependency(SimulationExecutionPoint::hook(kProbeSystem, kTickHook),
            SimulationExecutionPoint::task(kProbeSystem, kPropagate)));
        assert(addScriptSystemData(builder, script_description, codec_limits));
        auto result = std::move(builder).build();
        assert(result);
        return std::move(*result);
    }

    [[nodiscard]] ScriptSystemDescription makeScriptDescription()
    {
        SimulationDescriptionBuilder simulation_builder;
        assert(simulation_builder.addSystem(kProbeSystem, "probe", ProbeSystem::Description));
        auto simulation = std::move(simulation_builder).build();
        assert(simulation);

        ScriptSystemDescriptionBuilder builder;
        assert(builder.addMount({
            ScriptMountId{1U},
            assetId(0x72U),
            SimulationScriptMount{},
            true,
            {{kTickSymbol, HookScriptTarget{kProbeSystem, kTickHook}}}
        }));
        assert(builder.addMount({
            ScriptMountId{2U},
            assetId(0x72U),
            SimulationScriptMount{},
            true,
            {{kTickSymbol, HookScriptTarget{kProbeSystem, kTickHook}}}
        }));
        // A long-lived pending object must not prevent the stable Hook or final derived propagation.
        assert(builder.addMount({
            ScriptMountId{3U},
            assetId(0x72U),
            EntityScriptMount{worldId<lux::world::WorldObjectId>(0xf1U)},
            true,
            {{kTickSymbol, HookScriptTarget{kProbeSystem, kTickHook}}}
        }));
        auto result = std::move(builder).build(*simulation);
        assert(result);
        return std::move(*result);
    }

    [[nodiscard]] lux::script::ScriptArtifact makeArtifact()
    {
        rdesc::Script description;
        description.module_name = "lux.test.scene-script.fixture";
        description.exports.push_back({"tick", kTickSymbol, {}, {}});
        description.api_requirements.push_back({
            lux::script::ScriptApiContractId{DelayAbilityTraits::Description.id.name()},
            DelayAbilityTraits::Description.schema_hash
        });
        description.body = rdesc::CppStaticScript{"fixture"};
        auto result = lux::script::ScriptArtifact::create(std::move(description), {});
        assert(result);
        return std::move(*result);
    }

    struct BackendState final
    {
        enum class EDelayMode : std::uint8_t
        {
            NEXT_STEP,
            SECONDS,
            SIMULATION_SECONDS,
            REAL_SECONDS,
        };

        std::size_t step_calls{};
        std::size_t resume_calls{};
        std::size_t destroys{};
        EDelayMode delay_mode{EDelayMode::NEXT_STEP};
        double delay_seconds{};
        std::optional<lux::script::ScriptAbilityStarter<DelayAbility>> delay;
    };

    struct StableProbeSystem final
    {
        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "lux.test.scene-script.stable-probe",
            .version = 1U
        };

        explicit StableProbeSystem(BackendState& state) noexcept : state_(&state)
        {
        }

        void executeStablePoint() noexcept
        {
            observed_resume_calls = state_->resume_calls;
            observed_derived = active_probe->derived_value;
            assert(observed_derived == state_->resume_calls * 10U);
        }

        BackendState* state_{};
        std::size_t observed_resume_calls{};
        std::size_t observed_derived{};
    };

    [[nodiscard]] lux::cxx::expected<void, SceneSystemBuildFailure> installStableProbe(
        SceneBuilder& builder,
        SceneSystemView description
    ) noexcept
    {
        auto* state = builder.require<BackendState>(description.instanceId(), "backend_state");
        if (state == nullptr)
        {
            return lux::cxx::unexpected(SceneSystemBuildFailure{
                ESceneSystemBuildError::MISSING_REQUIREMENT,
                description.instanceId()
            });
        }
        auto installed = builder.emplaceSystem<StableProbeSystem>(description.instanceId(), *state);
        if (!installed)
            return lux::cxx::unexpected(installed.error());
        return builder.addStablePointTask<StableProbeSystem>(
            description.instanceId(),
            [](StableProbeSystem& value) noexcept { value.executeStablePoint(); }
        );
    }

    [[nodiscard]] SceneSystemRegistration stableProbeRegistration() noexcept
    {
        static constexpr std::array requirements{
            SceneSystemRequirementSpec{
                .name = "backend_state",
                .capability = "lux.test.script.backend-state",
                .expected_type = cxx::typeToken<BackendState>(),
                .optional = false
            }
        };
        return {
            .type = system::systemTypeId(StableProbeSystem::Description.canonical_name),
            .cpp_type = cxx::typeToken<StableProbeSystem>(),
            .description = &StableProbeSystem::Description,
            .configuration = {},
            .observations = {},
            .requirements = requirements,
            .connections = {},
            .project_object = sceneSystemObjectProjection<StableProbeSystem>(),
            .install = &installStableProbe
        };
    }

    struct Continuation final
    {
        BackendState* owner{};
    };

    int invokeSync(lux_script_call_frame*)
    {
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
        if (create.capabilities.size() != 1U)
            return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
        const auto& capability = create.capabilities.front();
        const lux::script::ScriptAbilityBinding binding{
            &DelayAbilityTraits::Description,
            capability.context,
            capability.dispatch
        };
        auto starter = lux::script::ScriptAbilityStarter<DelayAbility>::create(binding);
        if (!starter)
            return EScriptBackendResult::CONSTRUCTION_FAILURE;
        state.delay = std::move(*starter);
        output.value = context;
        return EScriptBackendResult::SUCCESS;
    }

    ScriptStepResult invokeStep(
        void* context,
        lux_script_call_frame&,
        ScriptStepContext& step,
        ScriptBackendContinuation& output
    ) noexcept;

    EScriptBackendResult prepareMethod(
        void* context,
        ScriptBackendInstance,
        const rdesc::ScriptFunction&,
        ScriptBackendPreparedMethod& output
    ) noexcept
    {
        output = {
            context,
            lux::script::BoundScriptCall{&invokeSync, nullptr},
            BoundScriptStepCall{context, &invokeStep}
        };
        return EScriptBackendResult::SUCCESS;
    }

    void releaseMethod(void*, ScriptBackendInstance, ScriptBackendPreparedMethod) noexcept
    {
    }

    void destroyInstance(void*, ScriptBackendInstance) noexcept
    {
    }

    ScriptStepResult resume(void* value, ScriptStepContext&, const ScriptResumePacket& packet) noexcept
    {
        auto& continuation = *static_cast<Continuation*>(value);
        assert(packet.state == EScriptAwaitableState::READY);
        ++continuation.owner->resume_calls;
        ++active_probe->live_value;
        return ScriptStepResult::completed();
    }

    void destroyContinuation(void* value) noexcept
    {
        auto* continuation = static_cast<Continuation*>(value);
        ++continuation->owner->destroys;
        delete continuation;
    }

    ScriptStepResult invokeStep(
        void* context,
        lux_script_call_frame&,
        ScriptStepContext& step,
        ScriptBackendContinuation& output
    ) noexcept
    {
        auto& state = *static_cast<BackendState*>(context);
        ++state.step_calls;
        assert(state.delay.has_value());
        auto* continuation = new (std::nothrow) Continuation{&state};
        if (continuation == nullptr)
            return ScriptStepResult::failed(2);
        auto result = [&]() noexcept {
            switch (state.delay_mode)
            {
            case BackendState::EDelayMode::NEXT_STEP:
                return invokeScriptAbilityAsync<void>(
                    step,
                    [&state](lux::script::ScriptAbilityCompletion<void> completion) noexcept {
                        return state.delay->nextStep(std::move(completion));
                    }
                );
            case BackendState::EDelayMode::SECONDS:
                return invokeScriptAbilityAsync<void>(
                    step,
                    [&state](lux::script::ScriptAbilityCompletion<void> completion) noexcept {
                        return state.delay->seconds(state.delay_seconds, std::move(completion));
                    }
                );
            case BackendState::EDelayMode::SIMULATION_SECONDS:
                return invokeScriptAbilityAsync<void>(
                    step,
                    [&state](lux::script::ScriptAbilityCompletion<void> completion) noexcept {
                        return state.delay->simulationSeconds(state.delay_seconds, std::move(completion));
                    }
                );
            case BackendState::EDelayMode::REAL_SECONDS:
                return invokeScriptAbilityAsync<void>(
                    step,
                    [&state](lux::script::ScriptAbilityCompletion<void> completion) noexcept {
                        return state.delay->realSeconds(state.delay_seconds, std::move(completion));
                    }
                );
            }
            return ScriptStepResult::failed(3);
        }();
        if (result.state != EScriptStepState::SUSPENDED)
        {
            delete continuation;
            return result;
        }
        output = {continuation, &resume, &destroyContinuation};
        return result;
    }

    struct Fixture final
    {
        Fixture() : artifact(makeArtifact())
        {
            backend = {
                rdesc::Script::Kind::CPP_STATIC,
                &backend_state,
                &createInstance,
                &prepareMethod,
                &releaseMethod,
                &destroyInstance
            };
        }

        static bool resolveArtifact(
            void* context,
            const asset::AssetId& requested,
            ResolvedScriptArtifact& output
        ) noexcept
        {
            auto& self = *static_cast<Fixture*>(context);
            if (requested != assetId(0x72U))
                return false;
            output.artifact = &self.artifact;
            return true;
        }

        static bool resolveWorld(void* context, const world::WorldObjectId& object, ecs::Entity& entity) noexcept
        {
            auto& self = *static_cast<Fixture*>(context);
            ++self.resolve_calls;
            if (object != worldId<world::WorldObjectId>(0xf1U) || self.resolved_entity == ecs::NullEntity)
                return false;
            entity = self.resolved_entity;
            return true;
        }

        ecs::Entity resolved_entity{ecs::NullEntity};
        std::size_t resolve_calls{};
        BackendState backend_state;
        lux::script::ScriptArtifact artifact;
        ScriptBackendDescriptor backend;
    };

    [[nodiscard]] std::shared_ptr<const world::WorldDescription> makeWorld()
    {
        world::WorldDescriptionBuilder builder;
        assert(builder.setIdentity(
            worldId<world::WorldBundleId>(1U),
            worldId<world::WorldBundleGeneration>(2U),
            "script-runtime-test"
        ));
        assert(builder.setPartitioner({world::worldPartitionerId("test.none"), 1U}, 0U));
        auto world = std::move(builder).build();
        assert(world);
        return std::make_shared<world::WorldDescription>(std::move(*world));
    }
} // namespace

int main()
{
    using namespace lux;
    using namespace lux::scene;
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    using namespace lux::scene::script;

    const ScriptSystemCodecLimits codec_limits{4096U, 4096U, 4096U};
    auto script_description = makeScriptDescription();
    auto simulation = std::make_shared<SimulationDescription>(
        makeSimulationDescription(script_description, codec_limits)
    );

    SceneDescriptionBuilder scene_builder;
    scene_builder.setWorld(assetId(1U));
    scene_builder.setSimulation(assetId(2U));
    assert(scene_builder.addSystem(
        kScriptRuntime,
        "script-runtime",
        system::systemTypeId(ScriptRuntimeSystem::Description.canonical_name),
        ScriptRuntimeSystem::Description.version,
        {},
        0U
    ));
    assert(scene_builder.bindRequirement(kScriptRuntime, "script_runtime_host", "host.script"));
    assert(scene_builder.bindRequirement(kScriptRuntime, "timer", "host.timer"));
    assert(scene_builder.addSystem(
        kStableProbe,
        "stable-probe",
        system::systemTypeId(StableProbeSystem::Description.canonical_name),
        StableProbeSystem::Description.version,
        {},
        0U
    ));
    assert(scene_builder.bindRequirement(kStableProbe, "backend_state", "host.backend-state"));
    assert(scene_builder.addDependency(kScriptRuntime, kStableProbe));
    auto scene_description = std::move(scene_builder).build();
    assert(scene_description);

    meta::ReflectionRegistry::initRegistry();
    SimulationSystemRegistry simulation_systems;
    assert(simulation_systems.add(probeRegistration()));
    auto components = ecs::ComponentSchemaSet::build({});
    assert(components);
    auto meta = SceneMetaManager::build({
        std::move(*components),
        std::move(simulation_systems),
        {builtinScriptRuntimeSystemRegistration(), stableProbeRegistration()}
    });
    assert(meta);

    Fixture fixture;
    const std::array backends{fixture.backend};
    ScriptRuntimeHost host{
        ScriptRuntimeLimits{8U, 3U, 4U, 2U, 4U, 4U, 64U, 1U, 4U, 4U, 4U, 4U},
        codec_limits,
        4U,
        {&fixture, &Fixture::resolveArtifact},
        {&fixture, &Fixture::resolveWorld},
        backends,
        {}
    };
    const auto provider = makeSceneCapabilityProvider<ScriptRuntimeHost>(
        "host.script",
        "lux.script.runtime.host",
        host
    );
    const auto backend_provider = makeSceneCapabilityProvider<BackendState>(
        "host.backend-state",
        "lux.test.script.backend-state",
        fixture.backend_state
    );
    auto execution = process::ExecutionRuntime::create({1U, 8U, 8U, {8U}, std::nullopt});
    assert(execution);
    auto timer = execution->timer();
    const auto timer_provider = makeSceneCapabilityProvider<process::TimerClient>(
        "host.timer",
        "lux.process.timer",
        timer
    );
    const std::array providers{provider, backend_provider, timer_provider};
    auto scene = Scene::create({
        std::make_shared<SceneDescription>(std::move(*scene_description)),
        makeWorld(),
        simulation,
        *meta,
        providers
    });
    assert(scene);
    auto* runtime = (*scene)->findSceneSystem<ScriptRuntimeSystem>();
    assert(runtime != nullptr);

    auto executor = task::TaskExecutor::create({1U, 8U});
    assert(executor);
    assert((*scene)->simulation().execute(*executor, SimulationDuration{1}));
    assert(fixture.backend_state.step_calls == 2U);
    assert(fixture.backend_state.resume_calls == 0U);
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 0U);
    assert((*scene)->simulation().execute(*executor, SimulationDuration{1}));
    assert(fixture.backend_state.step_calls == 2U);
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 1U);
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 1U);
    assert((*scene)->simulation().execute(*executor, SimulationDuration{}));
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 2U);
    assert(fixture.backend_state.destroys == 2U);
    const auto* stable_probe = (*scene)->findSceneSystem<StableProbeSystem>();
    assert(stable_probe != nullptr && stable_probe->observed_resume_calls == 2U);
    assert(stable_probe->observed_derived == 20U);

    fixture.backend_state.delay_mode = BackendState::EDelayMode::SECONDS;
    active_probe->tick_requested = true;
    fixture.backend_state.delay_seconds = 3.0e-9;
    assert((*scene)->simulation().execute(*executor, SimulationDuration{1}));
    assert(fixture.backend_state.step_calls == 4U);
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 2U);
    assert((*scene)->simulation().execute(*executor, SimulationDuration{}));
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 2U);
    assert((*scene)->simulation().execute(*executor, SimulationDuration{2}));
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 2U);
    assert((*scene)->simulation().execute(*executor, SimulationDuration{1}));
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 3U);
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 3U);
    assert((*scene)->simulation().execute(*executor, SimulationDuration{}));
    assert(fixture.backend_state.resume_calls == 4U);

    fixture.backend_state.delay_mode = BackendState::EDelayMode::SIMULATION_SECONDS;
    active_probe->tick_requested = true;
    fixture.backend_state.delay_seconds = 0.0;
    assert((*scene)->simulation().execute(*executor, SimulationDuration{}));
    assert(fixture.backend_state.step_calls == 6U);
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 4U);
    assert((*scene)->simulation().execute(*executor, SimulationDuration{}));
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 5U);
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 5U);
    assert((*scene)->simulation().execute(*executor, SimulationDuration{}));
    assert(fixture.backend_state.resume_calls == 6U);

    fixture.backend_state.delay_mode = BackendState::EDelayMode::REAL_SECONDS;
    active_probe->tick_requested = true;
    fixture.backend_state.delay_seconds = 0.02;
    assert((*scene)->simulation().execute(*executor, SimulationDuration{1}));
    assert(fixture.backend_state.step_calls == 8U);
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 6U);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(fixture.backend_state.resume_calls == 6U);
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 6U);
    assert((*scene)->simulation().execute(*executor, SimulationDuration{}));
    assert(fixture.backend_state.resume_calls == 7U);
    assert((*scene)->simulation().execute(*executor, SimulationDuration{}));
    assert(fixture.backend_state.resume_calls == 8U);

    fixture.backend_state.delay_seconds = 0.0;
    active_probe->tick_requested = true;
    assert((*scene)->simulation().execute(*executor, SimulationDuration{}));
    assert(fixture.backend_state.step_calls == 10U);
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 8U);
    assert((*scene)->simulation().execute(*executor, SimulationDuration{}));
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 9U);
    assert((*scene)->executeStablePoint());
    assert(fixture.backend_state.resume_calls == 9U);
    assert((*scene)->simulation().execute(*executor, SimulationDuration{}));
    assert(fixture.backend_state.resume_calls == 10U);

    fixture.backend_state.delay_seconds = 1000.0;
    active_probe->tick_requested = true;
    assert((*scene)->simulation().execute(*executor, SimulationDuration{}));
    assert(fixture.backend_state.step_calls == 12U);

    // The original unresolved description is loader-owned. Resolve it at an existing before boundary.
    assert(runtime->scriptSystem().activeInstanceCount() == 2U);
    fixture.resolved_entity = (*scene)->registry().create();
    assert((*scene)->simulation().execute(*executor, SimulationDuration{}));
    assert(runtime->scriptSystem().activeInstanceCount() == 3U);
    auto mounted = runtime->scriptSystem().queryMountStatus({3U});
    assert(mounted && *mounted && (**mounted).state == EScriptMountState::ACTIVE);
    const auto old_instance = (**mounted).instance;
    const auto resolved_calls = fixture.resolve_calls;
    assert((*scene)->simulation().execute(*executor, SimulationDuration{}));
    assert(fixture.resolve_calls == resolved_calls); // No scan/re-resolution of ACTIVE configurations.
    const auto old_entity = fixture.resolved_entity;
    (*scene)->registry().destroy(old_entity);
    fixture.resolved_entity = (*scene)->registry().create();
    assert((*scene)->simulation().execute(*executor, SimulationDuration{}));
    mounted = runtime->scriptSystem().queryMountStatus({3U});
    assert(mounted && *mounted && (**mounted).state == EScriptMountState::ACTIVE);
    assert((**mounted).instance != old_instance);
    assert(std::get<EntityScriptScope>((**mounted).scope).self == fixture.resolved_entity);
    assert(runtime->scriptSystem().activeInstanceCount() == 3U);
    assert(fixture.backend_state.step_calls == 12U && fixture.backend_state.resume_calls == 10U);

    scene->reset();
    assert(fixture.backend_state.resume_calls == 10U);
    assert(fixture.backend_state.destroys == 12U);
    execution->requestStop();
    assert(execution->join());
    meta::ReflectionRegistry::destroyRegistry();
    return 0;
}
