#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>

#include <array>
#include <cassert>
#include <iostream>
#include <thread>

namespace
{
    using namespace lux;
    using namespace lux::scene;
    using namespace std::chrono_literals;
    namespace ecs = simulation::ecs;

    struct State final
    {
        bool waiting{}, blocked{}, failed{}, alive{true};
        SceneRuntime* runtime{};
        SceneInstanceId id;
        std::size_t maintained{}, published{};
        ~State()
        {
            assert(!alive);
        }
    };
    struct Probe final
    {
        inline static constexpr std::string_view SupportedWorldTypes[]{"*"};
        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "test.runtime.probe",
            .version = 1,
            .supported_world_types = SupportedWorldTypes
        };
        inline static bool reject{};
        inline static unsigned destroyed{};
        State& state;
        explicit Probe(ecs::Registry& registry) : state(registry.ctx().emplace<State>()) {}
        ~Probe()
        {
            state.alive = false;
            ++destroyed;
        }
    };
    SceneSystemRegistration probeRegistration()
    {
        return {
            .type = system::systemTypeId(Probe::Description.canonical_name),
            .cpp_type = cxx::typeToken<Probe>(),
            .description = &Probe::Description,
            .install = +[](SceneSystemInstaller& installer, SceneSystemDescription description
                        ) noexcept -> cxx::expected<void, SceneSystemBuildFailure> {
                auto probe = installer.emplaceSystem<Probe>(description.instanceId(), installer.registry());
                if (!probe)
                    return cxx::unexpected(probe.error());
                if (Probe::reject)
                    return cxx::unexpected(SceneSystemBuildFailure{
                        .code = ESceneSystemBuildError::EXTERNAL_OPERATION_FAILURE,
                        .system = description.instanceId(),
                        .cause = 419
                    });
                const auto maintenance = installer.addMaintenanceTask<Probe>(
                    description.instanceId(),
                    [](Probe& probe) noexcept -> SceneStageResult {
                        ++probe.state.maintained;
                        if (probe.state.runtime)
                        {
                            const auto reentrant = probe.state.runtime->getClock(probe.state.id);
                            assert(
                                !reentrant &&
                                std::get<ESceneRuntimeError>(reentrant.error().cause) == ESceneRuntimeError::BUSY
                            );
                        }
                        return probe.state.waiting ? ESceneProgress::PENDING : ESceneProgress::COMPLETE;
                    }
                );
                if (!maintenance)
                    return maintenance;
                return installer.addPublicationTask<Probe>(
                    description.instanceId(),
                    [](Probe& probe) noexcept -> SceneStageResult {
                        ++probe.state.published;
                        if (probe.state.failed)
                            return cxx::unexpected(SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, 731}
                            );
                        return probe.state.blocked ? ESceneProgress::PENDING : ESceneProgress::COMPLETE;
                    }
                );
            }
        };
    }

    simulation::SimulationTime time(const SceneRuntime& runtime, SceneInstanceId id)
    {
        const auto clock = runtime.getClock(id);
        assert(clock);
        return std::visit([](const auto& value) { return value.snapshot(); }, clock->get());
    }
}

int main()
{
    auto execution = process::ExecutionRuntime::create({1, 32, 32, {16}});
    assert(execution);
    auto runtime = SceneRuntime::create(*execution, {0, 1024});
    auto other = SceneRuntime::create(*execution, {1, 1024});
    assert(runtime && other);
    const ecs::ComponentSchemaSet components;
    const simulation::SimulationSystemRegistry simulation_systems;
    const std::array registrations{probeRegistration(), transformSystemRegistration()};
    SceneDescriptionBuilder description;
    assert(description.addSystem({1}, "probe", registrations[0].type, 1, {}, 0));
    const auto config = makeTransformSystemConfiguration(64, {512, 65536});
    assert(config);
    assert(description.addSystem(
        {2},
        "transform",
        registrations[1].type,
        1,
        registrations[1].description->configuration_schema_name,
        1,
        *config
    ));
    auto resolved = std::move(description).buildResolved();
    assert(resolved);
    auto builder = (*runtime)->builder();
    builder.setDescription(std::make_shared<const SceneDescription>(std::move(*resolved)))
        .setWorld(std::make_shared<const world::WorldDescription>())
        .setSimulation(std::make_shared<const simulation::SimulationDescription>())
        .setRegistrations(components, simulation_systems, registrations);
    assert(!(*runtime)->builder().build());
    Probe::reject = true;
    const auto rejected = builder.build();
    assert(!rejected && Probe::destroyed == 1);
    assert(std::any_cast<int>(std::get<SceneBuildFailure>(rejected.error().cause).scene_system.cause) == 419);
    Probe::reject = false;
    auto slow = FixedStepClock::create(1h);
    assert(slow);
    builder.setClock(*slow);
    const auto first = builder.build(), second = builder.build();
    assert(first && second && *first != *second);
    assert(!(*other)->getClock(*first));
    assert((*runtime)->invalid(*first));
    auto registry = (*runtime)->getSceneRegistry(*first);
    assert(registry);
    const auto parent = registry->get().create(), child = registry->get().create();
    registry->get().emplace<ecs::Transform3D>(parent).translation.x() = 10;
    registry->get().emplace<ecs::Transform3D>(child).translation.x() = 2;
    registry->get().emplace<ecs::Parent>(child, parent);
    auto tick = (*runtime)->tick();
    assert(tick && tick->empty() && time(**runtime, *first).step_index == 0);
    assert(time(**runtime, *second).step_index == 1);
    registry = (*runtime)->getSceneRegistry(*first);
    assert(registry->get().get<ecs::WorldTransform3D>(child).value.translation().x() == 12);
    registry->get().patch<ecs::Transform3D>(parent, [](auto& value) { value.translation.x() = 20; });
    assert((*runtime)->tick());
    registry = (*runtime)->getSceneRegistry(*first);
    assert(registry->get().get<ecs::WorldTransform3D>(child).value.translation().x() == 22);

    auto& probe = registry->get().ctx().get<State>();
    probe.runtime = runtime->get();
    probe.id = *first;
    probe.waiting = true;
    assert((*runtime)->valid(*first));
    assert((*runtime)->tick() && time(**runtime, *first).step_index == 0);
    probe.waiting = false; // Controlled endpoint readiness; no Registry structural mutation.
    probe.blocked = true;
    assert((*runtime)->tick() && time(**runtime, *first).step_index == 1);
    assert(!(*runtime)->getSceneRegistry(*first));
    assert(std::as_const(**runtime).getSceneRegistry(*first));
    for (unsigned turn{}; turn < 5; ++turn)
        assert((*runtime)->tick());
    assert(time(**runtime, *first).step_index == 1);
    probe.blocked = false;
    assert((*runtime)->tick());
    assert((*runtime)->invalid(*first) && (*runtime)->valid(*first));
    assert((*runtime)->tick() && time(**runtime, *first).step_index == 2);
    assert((*runtime)->valid(*first) && (*runtime)->tick());
    assert(time(**runtime, *first).step_index == 2); // Repeated valid does not rebase again.

    probe.failed = true;
    assert((*runtime)->invalid(*first) && (*runtime)->valid(*first));
    tick = (*runtime)->tick();
    assert(tick && tick->size() == 1 && tick->front().scene == *first);
    assert(std::get<SceneDriveFailure>(tick->front().cause).phase == ESceneDrivePhase::PUBLICATION);
    assert(!(*runtime)->valid(*first) && time(**runtime, *first).step_index == 3);
    assert((*runtime)->invalid(*second) && (*runtime)->valid(*second));
    assert((*runtime)->tick() && time(**runtime, *second).step_index == 2);

    bool wrong_thread{};
    std::jthread worker([&] {
        const auto rejected = (*runtime)->getClock(*second);
        wrong_thread =
            !rejected && std::get<ESceneRuntimeError>(rejected.error().cause) == ESceneRuntimeError::WRONG_THREAD;
    });
    worker.join();
    assert(wrong_thread);
    assert((*runtime)->destroy(*first));
    const auto replacement = builder.build();
    assert(replacement && replacement->slot == first->slot && replacement->generation != first->generation);
    assert(!(*runtime)->getClock(*first) && (*runtime)->destroy(*first));
    assert((*runtime)->getClock(*replacement));
    assert((*runtime)->destroy(*replacement) && (*runtime)->destroy(*second));

    auto overflowing = FixedStepClock::create(simulation::SimulationDuration::max());
    assert(overflowing);
    builder.setClock(*overflowing);
    const auto overflowed = builder.build();
    assert(overflowed);
    tick = (*runtime)->tick();
    assert(tick && tick->size() == 1 && tick->front().scene == *overflowed);
    assert(std::get<EClockError>(tick->front().cause) == EClockError::TIME_OVERFLOW);
    assert(time(**runtime, *overflowed).step_index == 0 && !(*runtime)->valid(*overflowed));
    assert((*runtime)->destroy(*overflowed));

    // Timer completion and cancellation do not create business Task records.
    builder.setClock(FixedStepClock{});
    const auto timed = builder.build();
    assert(timed && (*runtime)->tick());
    const auto start = std::chrono::steady_clock::now();
    while (time(**runtime, *timed).step_index < 3)
    {
        const auto epoch = execution->wakeEpoch();
        assert(execution->collectCompletions());
        assert((*runtime)->tick());
        assert(std::chrono::steady_clock::now() - start < 2s);
        if (time(**runtime, *timed).step_index < 3)
            execution->waitForWork(epoch, start + 2s);
    }
    assert(execution->taskInfos().empty());
    assert((*runtime)->invalid(*timed));
    for (unsigned warmup{}; warmup < 8; ++warmup)
    {
        assert(execution->collectCompletions());
        assert((*runtime)->tick());
    }
    const auto measured = std::chrono::steady_clock::now();
    for (unsigned turn{}; turn < 10000; ++turn)
        assert((*runtime)->tick());
    const auto elapsed = std::chrono::steady_clock::now() - measured;
    std::cout << "MEASURE SceneRuntime paused_turns=10000 elapsed_us="
              << std::chrono::duration<double, std::micro>(elapsed).count() << " business_tasks=0\n";
    assert((*runtime)->valid(*timed) && (*runtime)->tick());
    runtime->reset(); // Outstanding timer must be cancelled and joined before receiver storage is reclaimed.
    other->reset();
    assert(execution->collectCompletions());
    std::cout << "PASS SceneRuntime: identity, sealed construction, multi-scene clocks, pause, failure, RAII timer\n";
}
