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
        SceneInstanceLease* retire_owner{};
        InstanceRetirement* retirement{};
        bool* in_flight{};
        ~State()
        {
            assert(!alive);
        }
    };
    struct OwnedFailure final
    {
        std::weak_ptr<const void> code;
        ~OwnedFailure()
        {
            assert(!code.expired());
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
        inline static std::weak_ptr<const void> error_code;
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
                            if (probe.state.retire_owner)
                            {
                                const auto destroyed = Probe::destroyed;
                                *probe.state.retirement = probe.state.retire_owner->retire();
                                probe.state.retire_owner = nullptr;
                                assert(!probe.state.retirement->complete() && Probe::destroyed == destroyed);
                                // Leaf retirement must not clear the enclosing runtime traversal guard.
                                assert(!probe.state.runtime->driveFrame());
                            }
                            const auto reentrant = probe.state.runtime->borrowClock(probe.state.id);
                            assert(
                                !reentrant &&
                                std::get<ESceneRuntimeError>(reentrant.error().cause) == ESceneRuntimeError::BUSY
                            );
                        }
                        const bool pending = probe.state.waiting || (probe.state.in_flight && *probe.state.in_flight);
                        return pending ? ESceneProgress::PENDING : ESceneProgress::COMPLETE;
                    }
                );
                if (!maintenance)
                    return maintenance;
                return installer.addPublicationTask<Probe>(
                    description.instanceId(),
                    [](Probe& probe) noexcept -> SceneStageResult {
                        ++probe.state.published;
                        if (probe.state.failed && !Probe::error_code.expired())
                            return cxx::unexpected(SceneExecutionFailure{
                                ESceneExecutionError::SYSTEM_FAILURE,
                                {},
                                OwnedFailure{Probe::error_code}
                            });
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
        const auto clock = runtime.borrowClock(id);
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
    assert(first && second && first->id() != second->id());
    assert(!(*other)->borrowClock(first->id()));
    assert((*runtime)->pauseSimulation(first->id()));
    auto registry = (*runtime)->borrowInstance(first->id());
    assert(registry);
    const auto parent = registry->get().create(), child = registry->get().create();
    registry->get().emplace<ecs::Transform3D>(parent).translation.x() = 10;
    registry->get().emplace<ecs::Transform3D>(child).translation.x() = 2;
    registry->get().emplace<ecs::Parent>(child, parent);
    auto tick = (*runtime)->driveFrame();
    assert(tick && tick->empty() && time(**runtime, first->id()).step_index == 0);
    assert(time(**runtime, second->id()).step_index == 1);
    registry = (*runtime)->borrowInstance(first->id());
    assert(registry->get().get<ecs::WorldTransform3D>(child).value.translation().x() == 12);
    registry->get().patch<ecs::Transform3D>(parent, [](auto& value) { value.translation.x() = 20; });
    assert((*runtime)->driveFrame());
    registry = (*runtime)->borrowInstance(first->id());
    assert(registry->get().get<ecs::WorldTransform3D>(child).value.translation().x() == 22);

    auto& probe = registry->get().ctx().get<State>();
    probe.runtime = runtime->get();
    probe.id = first->id();
    probe.waiting = true;
    assert((*runtime)->resumeSimulation(first->id()));
    assert((*runtime)->driveFrame() && time(**runtime, first->id()).step_index == 0);
    probe.waiting = false; // Controlled endpoint readiness; no Registry structural mutation.
    probe.blocked = true;
    assert((*runtime)->driveFrame() && time(**runtime, first->id()).step_index == 1);
    assert(!(*runtime)->borrowInstance(first->id()));
    assert(std::as_const(**runtime).borrowInstance(first->id()));
    for (unsigned turn{}; turn < 5; ++turn)
        assert((*runtime)->driveFrame());
    assert(time(**runtime, first->id()).step_index == 1);
    probe.blocked = false;
    assert((*runtime)->driveFrame());
    assert((*runtime)->pauseSimulation(first->id()) && (*runtime)->resumeSimulation(first->id()));
    assert((*runtime)->driveFrame() && time(**runtime, first->id()).step_index == 2);
    assert((*runtime)->resumeSimulation(first->id()) && (*runtime)->driveFrame());
    assert(time(**runtime, first->id()).step_index == 2); // Repeated valid does not rebase again.

    probe.failed = true;
    assert((*runtime)->pauseSimulation(first->id()) && (*runtime)->resumeSimulation(first->id()));
    tick = (*runtime)->driveFrame();
    assert(tick && tick->size() == 1 && tick->front().scene == first->id());
    assert(std::get<SceneDriveFailure>(tick->front().cause).phase == ESceneDrivePhase::PUBLICATION);
    assert(!(*runtime)->resumeSimulation(first->id()) && time(**runtime, first->id()).step_index == 3);
    assert((*runtime)->pauseSimulation(second->id()) && (*runtime)->resumeSimulation(second->id()));
    assert((*runtime)->driveFrame() && time(**runtime, second->id()).step_index == 2);

    bool wrong_thread{};
    std::jthread worker([&] {
        const auto rejected = (*runtime)->borrowClock(second->id());
        wrong_thread =
            !rejected && std::get<ESceneRuntimeError>(rejected.error().cause) == ESceneRuntimeError::WRONG_THREAD;
    });
    worker.join();
    assert(wrong_thread);
    const auto first_retirement = (*runtime)->retireInstance(first->id());
    assert(first_retirement && !first_retirement->complete());
    assert((*runtime)->borrowClock(first->id()));
    assert((*runtime)->driveFrame() && first_retirement->complete());
    const auto replacement = builder.build();
    assert(
        replacement && replacement->id().slot == first->id().slot &&
        replacement->id().generation != first->id().generation
    );
    assert(!(*runtime)->borrowClock(first->id()) && first_retirement->complete());
    assert(!(*runtime)->retireInstance(first->id())); // A stale slot cannot retire its replacement.
    assert((*runtime)->borrowClock(replacement->id()));
    assert((*runtime)->retireInstance(replacement->id()) && (*runtime)->retireInstance(second->id()));
    assert((*runtime)->driveFrame());

    auto overflowing = FixedStepClock::create(simulation::SimulationDuration::max());
    assert(overflowing);
    builder.setClock(*overflowing);
    const auto overflowed = builder.build();
    assert(overflowed);
    tick = (*runtime)->driveFrame();
    assert(tick && tick->size() == 1 && tick->front().scene == overflowed->id());
    assert(std::get<EClockError>(tick->front().cause) == EClockError::TIME_OVERFLOW);
    assert(time(**runtime, overflowed->id()).step_index == 0 && !(*runtime)->resumeSimulation(overflowed->id()));
    assert((*runtime)->retireInstance(overflowed->id()));
    assert((*runtime)->driveFrame());

    // Timer completion and cancellation do not create business Task records.
    builder.setClock(FixedStepClock{});
    const auto timed = builder.build();
    assert(timed && (*runtime)->driveFrame());
    const auto start = std::chrono::steady_clock::now();
    while (time(**runtime, timed->id()).step_index < 3)
    {
        const auto epoch = execution->wakeEpoch();
        assert(execution->collectCompletions());
        assert((*runtime)->driveFrame());
        assert(std::chrono::steady_clock::now() - start < 2s);
        if (time(**runtime, timed->id()).step_index < 3)
            execution->waitForWork(epoch, start + 2s);
    }
    assert(execution->taskInfos().empty());
    assert((*runtime)->pauseSimulation(timed->id()));
    for (unsigned warmup{}; warmup < 8; ++warmup)
    {
        assert(execution->collectCompletions());
        assert((*runtime)->driveFrame());
    }
    const auto measured = std::chrono::steady_clock::now();
    for (unsigned turn{}; turn < 10000; ++turn)
        assert((*runtime)->driveFrame());
    const auto elapsed = std::chrono::steady_clock::now() - measured;
    std::cout << "MEASURE SceneRuntime paused_turns=10000 elapsed_us="
              << std::chrono::duration<double, std::micro>(elapsed).count() << " business_tasks=0\n";
    assert((*runtime)->resumeSimulation(timed->id()) && (*runtime)->driveFrame());

    auto callback_owned = builder.build();
    assert(callback_owned);
    const auto callback_id = callback_owned->id();
    assert((*runtime)->pauseSimulation(callback_id));
    const auto callback_step = (*runtime)->requestStep(callback_id);
    assert(callback_step);
    const auto destroyed_before = Probe::destroyed;
    InstanceRetirement retirement;
    bool in_flight{true};
    {
        auto& state = (*runtime)->borrowInstance(callback_id)->get().ctx().get<State>();
        state.runtime = runtime->get();
        state.id = callback_id;
        state.retire_owner = &*callback_owned;
        state.retirement = &retirement;
        state.in_flight = &in_flight;
    }
    assert((*runtime)->driveFrame());
    assert(retirement.id() == callback_id && !retirement.complete());
    assert(Probe::destroyed == destroyed_before);
    assert((*runtime)->driveFrame() && !retirement.complete());
    in_flight = false;
    assert((*runtime)->driveFrame() && retirement.complete());
    assert(Probe::destroyed == destroyed_before + 1 && !(*runtime)->borrowClock(callback_id));
    auto late = (*runtime)->stepStatus(*callback_step, retirement);
    assert(late && late->state == ESceneStepState::CANCELLED && !late->result);
    assert(std::get<ESceneRuntimeError>(late->result.error().cause) == ESceneRuntimeError::STOPPED);
    assert(!(*other)->stepStatus(*callback_step, retirement));
    bool wrong_result_thread{};
    std::jthread result_reader([&] {
        const auto result = (*runtime)->stepStatus(*callback_step, retirement);
        wrong_result_thread =
            !result && std::get<ESceneRuntimeError>(result.error().cause) == ESceneRuntimeError::WRONG_THREAD;
    });
    result_reader.join();
    assert(wrong_result_thread);
    assert((*runtime)->acknowledgeStep(*callback_step, retirement));
    assert(!(*runtime)->stepStatus(*callback_step, retirement));
    *callback_owned = SceneInstanceLease{};
    assert((*runtime)->driveFrame() && Probe::destroyed == destroyed_before + 1);
    std::cout << "PASS X06-04 callback lease release is deferred, in-flight drain, outer guard and one destruction\n";
    // Only the failure value's code pin survives acknowledgement and heavy instance destruction.
    auto code = std::make_shared<int>(42);
    const std::weak_ptr<const void> weak_code = code;
    auto pinned_registrations = registrations;
    pinned_registrations[0].code_lifetime = code;
    auto pinned_builder = builder;
    pinned_builder.setRegistrations(components, simulation_systems, pinned_registrations);
    auto pinned = pinned_builder.build();
    assert(pinned);
    const auto pinned_id = pinned->id();
    pinned_registrations[0].code_lifetime.reset();
    Probe::error_code = code;
    code.reset();
    assert((*runtime)->pauseSimulation(pinned_id));
    (*runtime)->borrowInstance(pinned_id)->get().ctx().get<State>().failed = true;
    const auto failed_step = (*runtime)->requestStep(pinned_id);
    assert(failed_step && (*runtime)->driveFrame());
    auto pin_receipt = pinned->retire();
    assert((*runtime)->driveFrame() && pin_receipt.complete());
    assert(!(*runtime)->borrowClock(pinned_id));
    auto kept = (*runtime)->stepStatus(*failed_step, pin_receipt);
    assert(kept && kept->state == ESceneStepState::FAILED);
    const auto& cause = std::get<SceneExecutionFailure>(std::get<SceneDriveFailure>(kept->result.error().cause).cause);
    assert(!std::any_cast<const OwnedFailure&>(cause.cause).code.expired());
    assert((*runtime)->acknowledgeStep(*failed_step, pin_receipt));
    assert(!weak_code.expired());
    assert((*runtime)->driveFrame()); // End the prior borrowed DriveResult span, keeping only the copied result.
    *kept = SceneStepStatus{};
    assert(weak_code.expired()); // Receipt still exists, but no confirmed result retains code.
    Probe::error_code.reset();
    std::cout
        << "PASS R06-R1 result receipt identity/thread checks and copied failure code lifetime after reclamation\n";
    runtime->reset(); // Outstanding timer must be cancelled and joined before receiver storage is reclaimed.
    other->reset();
    assert(execution->collectCompletions());
    std::cout << "PASS SceneRuntime: identity, sealed construction, multi-scene clocks, pause, failure, RAII timer\n";
}
