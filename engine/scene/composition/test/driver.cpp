#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/scene/detail/SceneDriver.hpp>
#include <lux/engine/scene/detail/SceneInstance.hpp>

#include <array>
#include <cassert>
#include <iostream>

namespace
{
    using namespace lux;
    using namespace lux::scene;
    struct Padding
    {
        std::uint64_t prefix[3]{};
    };
    struct Capability
    {
        inline static constexpr std::string_view SupportedWorldTypes[]{"*"};
        static constexpr system::SystemTypeDescription Description{
            .canonical_name = "test.capability",
            .version = 1,
            .supported_world_types = SupportedWorldTypes
        };
        std::uint64_t marker{42};
    };
    struct Probe final : Padding, Capability
    {
        inline static constexpr std::string_view SupportedWorldTypes[]{"*"};
        static constexpr system::SystemTypeDescription Description{
            .canonical_name = "test.driver.probe",
            .version = 1,
            .supported_world_types = SupportedWorldTypes
        };
        std::size_t maintenance{}, synchronization{}, stable{}, publication{};
        bool changed{};
        bool gate{}, failure{}, maintenance_waiting{};
        simulation::ecs::Registry& registry;
        simulation::ecs::Entity entity;
        static inline std::size_t destructions{};
        static inline bool reject_install{};
        Probe(simulation::ecs::Registry& value) : registry(value), entity(value.create()) {}
        ~Probe() noexcept
        {
            assert(registry.valid(entity)); // System dies before the Registry.
            ++destructions;
        }
    };
    SceneSystemRegistration registration()
    {
        static constexpr std::array projections{sceneSystemCapabilityProjection<Probe, Capability>()};
        return {
            .type = system::systemTypeId(Probe::Description.canonical_name),
            .cpp_type = cxx::typeToken<Probe>(),
            .description = &Probe::Description,
            .install = +[](SceneSystemInstaller& builder,
                           SceneSystemDescription input) noexcept -> cxx::expected<void, SceneSystemBuildFailure> {
                auto system = builder.emplaceSystem<Probe>(input.instanceId(), builder.registry());
                if (!system)
                {
                    return cxx::unexpected(system.error());
                }
                if (Probe::reject_install)
                {
                    return cxx::unexpected(SceneSystemBuildFailure{
                        .code = ESceneSystemBuildError::EXTERNAL_OPERATION_FAILURE,
                        .system = input.instanceId(),
                        .cause = error::makeError(
                            {"fixture.scene.failure",
                             "Test failure {0}",
                             error::ERecovery::PERMANENT,
                             {error::EArgument::UNSIGNED}},
                            {719}
                        )
                    });
                }
                auto maintenance =
                    builder.addMaintenanceTask<Probe>(input.instanceId(), [](Probe& self) noexcept -> SceneStageResult {
                        ++self.maintenance;
                        return self.maintenance_waiting ? ESceneProgress::PENDING : ESceneProgress::COMPLETE;
                    });
                if (!maintenance)
                {
                    return maintenance;
                }
                auto synchronized = builder.addSynchronizationTask<Probe>(
                    input.instanceId(),
                    [](Probe& self, SceneStageContext& context) noexcept -> SceneStageResult {
                        ++self.synchronization;
                        context.publication_needed |= std::exchange(self.changed, false);
                        return ESceneProgress::COMPLETE;
                    }
                );
                if (!synchronized)
                    return synchronized;
                auto stable =
                    builder.addStablePointTask<Probe>(input.instanceId(), [](Probe& self) noexcept -> SceneStageResult {
                        ++self.stable;
                        if (self.failure)
                        {
                            return cxx::unexpected(SceneExecutionFailure{
                                ESceneExecutionError::SYSTEM_FAILURE,
                                {},
                                error::makeError(
                                    {"fixture.scene.failure",
                                     "Test failure {0}",
                                     error::ERecovery::PERMANENT,
                                     {error::EArgument::UNSIGNED}},
                                    {713}
                                )
                            });
                        }
                        return ESceneProgress::COMPLETE;
                    });
                if (!stable)
                {
                    return stable;
                }
                return builder.addPublicationTask<Probe>(
                    input.instanceId(),
                    [](Probe& self, SceneStageContext& context) noexcept -> SceneStageResult {
                        ++self.publication;
                        return self.gate ? ESceneProgress::COMPLETE : ESceneProgress::PENDING;
                    }
                );
            },
            .projections = projections
        };
    }
    struct IndependentProbe final
    {
        inline static constexpr std::string_view SupportedWorldTypes[]{"*"};
        static constexpr system::SystemTypeDescription Description{
            .canonical_name = "test.driver.independent",
            .version = 1,
            .supported_world_types = SupportedWorldTypes
        };
        std::size_t maintenance{};
    };
    SceneSystemRegistration independentRegistration()
    {
        return {
            .type = system::systemTypeId(IndependentProbe::Description.canonical_name),
            .cpp_type = cxx::typeToken<IndependentProbe>(),
            .description = &IndependentProbe::Description,
            .install = +[](SceneSystemInstaller& builder,
                           SceneSystemDescription input) noexcept -> cxx::expected<void, SceneSystemBuildFailure> {
                auto system = builder.emplaceSystem<IndependentProbe>(input.instanceId());
                if (!system)
                    return cxx::unexpected(system.error());
                return builder.addMaintenanceTask<IndependentProbe>(
                    input.instanceId(),
                    [](IndependentProbe& self) noexcept -> SceneStageResult {
                        ++self.maintenance;
                        return ESceneProgress::COMPLETE;
                    }
                );
            }
        };
    }
} // namespace

void runTransformChecks();

int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::scene;
    using namespace std::chrono_literals;
    runTransformChecks();
    const simulation::ecs::ComponentSchemaSet components;
    const simulation::SimulationSystemRegistry systems;
    const std::array registrations{registration(), independentRegistration()};
    SceneDescriptionBuilder builder;
    assert(builder.addSystem({1}, "probe", registration().type, 1, {}, 0));
    assert(builder.addSystem({2}, "independent", independentRegistration().type, 1, {}, 0));
    auto invalid_disk = std::move(builder).build();
    assert(!invalid_disk && invalid_disk.error().code == ESceneDescriptionError::INVALID_WORLD);
    auto description = std::move(builder).buildResolved();
    assert(description);
    auto shared = std::make_shared<const SceneDescription>(std::move(*description));
    SceneCreateInfo info{
        shared,
        std::make_shared<const world::WorldDescription>(),
        std::make_shared<const simulation::SimulationDescription>(),
        components,
        systems,
        registrations,
        {}
    };
    auto first = SceneInstance::create(info);
    auto second = SceneInstance::create(info);
    assert(first && second && (*first)->id() != (*second)->id());
    assert((*first)->simulation().seal());
    assert((*second)->simulation().seal());
    Probe::reject_install = true;
    auto rejected = SceneInstance::create(info);
    assert(!rejected && rejected.error().code == ESceneBuildError::SCENE_SYSTEM_BUILD_FAILURE);
    assert(rejected.error().scene_system.code == ESceneSystemBuildError::EXTERNAL_OPERATION_FAILURE);
    assert(rejected.error().scene_system.cause.args[0] == 719);
    assert(Probe::destructions == 1);
    Probe::reject_install = false;
    auto executor = task::TaskExecutor::create({0, 1024});
    assert(executor);
    SceneDriver driver(*executor);
    auto& instance = **first;
    auto& probe = *instance.findSceneSystem<Probe>();
    auto* capability = instance.findSceneSystem<Capability>();
    assert(capability == static_cast<Capability*>(&probe));
    assert(static_cast<void*>(capability) != static_cast<void*>(&probe));
    auto pending = driver.tick(instance, 16ms);
    assert(pending && *pending == ESceneTickResult::DEFERRED);
    assert(instance.progress().time.step_index == 0);
    assert(driver.maintain(instance) == ESceneProgress::COMPLETE);
    assert(probe.stable == 0 && probe.publication == 0 && probe.synchronization == 0);
    assert(driver.publish(instance) == ESceneProgress::PENDING);
    assert(probe.stable == 1 && !instance.atSafePoint());
    for (int turn = 0; turn < 100; ++turn)
    {
        static_cast<void>(driver.maintain(instance));
        assert(driver.publish(instance) == ESceneProgress::PENDING);
        assert(instance.progress().time.step_index == 0 && probe.stable == 1);
    }
    probe.gate = true;
    static_cast<void>(driver.maintain(instance));
    assert(driver.publish(instance) == ESceneProgress::COMPLETE && instance.atSafePoint());
    assert(probe.synchronization == 1);
    auto tick = driver.tick(instance, 16ms);
    assert(tick && *tick == ESceneTickResult::EXECUTED);
    assert(instance.progress().time.step_index == 1 && instance.progress().time.elapsed == 16ms);
    pending = driver.tick(instance, 32ms);
    assert(pending && *pending == ESceneTickResult::DEFERRED);
    probe.gate = false;
    static_cast<void>(driver.maintain(instance));
    assert(driver.publish(instance) == ESceneProgress::PENDING);
    for (int turn = 0; turn < 4; ++turn)
    {
        static_cast<void>(driver.maintain(instance));
        assert(driver.publish(instance) == ESceneProgress::PENDING);
        assert(instance.progress().time.step_index == 1 && probe.stable == 2);
    }
    probe.gate = true;
    static_cast<void>(driver.maintain(instance));
    assert(driver.publish(instance) == ESceneProgress::COMPLETE);
    assert(instance.progress().publication_completed == 1);
    probe.changed = true; // Synchronization consumes the edit after the next tick.
    tick = driver.tick(instance, 16ms);
    assert(tick && *tick == ESceneTickResult::EXECUTED);
    assert(instance.progress().time.step_index == 2 && instance.progress().time.delta == 0ns);
    static_cast<void>(driver.maintain(instance));
    assert(driver.publish(instance) == ESceneProgress::COMPLETE);
    assert(probe.synchronization == 3); // One synchronization per completed publication.
    auto invalid = driver.tick(instance, 8ms);
    assert(!invalid && std::get<ESceneDriveError>(invalid.error().cause) == ESceneDriveError::INVALID_TIME);
    assert(instance.progress().result && instance.progress().time.step_index == 2);
    probe.changed = true;
    static_cast<void>(driver.maintain(instance));
    assert(driver.publish(instance) == ESceneProgress::COMPLETE);
    assert(instance.progress().time.step_index == 2 && probe.synchronization == 4);
    probe.failure = true;
    tick = driver.tick(instance, 32ms);
    assert(tick && *tick == ESceneTickResult::EXECUTED);
    static_cast<void>(driver.maintain(instance));
    static_cast<void>(driver.publish(instance));
    assert(!instance.progress().result && instance.stopToken().stop_requested());
    assert(instance.progress().simulation_completed == 3 && instance.progress().stable_completed == 2);
    const auto& failure = std::get<SceneExecutionFailure>(instance.progress().result.error().cause);
    assert(
        failure.system.value == 1 && failure.cause.type == error::errorId("fixture.scene.failure") &&
        failure.cause.args[0] == 713
    );
    assert(!driver.tick(instance, 48ms));
    static_cast<void>(driver.maintain(instance));
    assert(driver.publish(instance) == ESceneProgress::COMPLETE);
    assert(std::get<SceneExecutionFailure>(instance.progress().result.error().cause).cause.args[0] == 713);
    assert((*second)->progress().time.step_index == 0);
    auto& other = *(*second)->findSceneSystem<Probe>();
    auto& independent = *(*second)->findSceneSystem<IndependentProbe>();
    other.gate = true;
    other.maintenance_waiting = true;
    static_cast<void>(driver.maintain(**second));
    assert(driver.publish(**second) == ESceneProgress::PENDING);
    assert(other.maintenance == 1 && independent.maintenance == 1);
    assert(other.stable == 1 && other.synchronization == 1);
    pending = driver.tick(**second, 16ms);
    assert(pending && *pending == ESceneTickResult::DEFERRED);
    static_cast<void>(driver.maintain(**second));
    assert(driver.publish(**second) == ESceneProgress::PENDING);
    assert(other.stable == 1 && independent.maintenance == 2);
    other.maintenance_waiting = false;
    static_cast<void>(driver.maintain(**second));
    assert(driver.publish(**second) == ESceneProgress::COMPLETE);
    (*second)->requestStop();
    static_cast<void>(driver.maintain(**second));
    assert(driver.publish(**second) == ESceneProgress::COMPLETE);
    assert(!driver.tick(**second, 16ms));
    first->reset();
    second->reset();
    assert(Probe::destructions == 3);
    std::cout << "PASS SceneDriver: explicit time, same-time execution, deferred admission, paused synchronization, "
                 "publication backpressure, failure and independent instances\n";
}
