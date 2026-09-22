#include <lux/engine/scene/SceneBuilder.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneInstance.hpp>

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
    static constexpr system::SystemTypeDescription Description{.canonical_name = "test.capability", .version = 1};
    std::uint64_t marker{42};
};
struct Probe final : Padding, Capability
{
    static constexpr system::SystemTypeDescription Description{.canonical_name = "test.driver.probe", .version = 1};
    std::size_t maintenance{}, stable{}, publication{};
    bool gate{}, failure{};
    simulation::ecs::Registry &registry;
    simulation::ecs::Entity entity;
    static inline std::size_t destructions{};
    static inline bool reject_install{};
    Probe(simulation::ecs::Registry &value) : registry(value), entity(value.create())
    {
    }
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
        .install =
            +[](SceneBuilder &builder, SceneSystemDescription input) noexcept -> cxx::expected<void, SceneSystemBuildFailure> {
            auto system = builder.emplaceSystem<Probe>(input.instanceId(), builder.registry());
            if (!system)
            {
                return cxx::unexpected(system.error());
            }
            if (Probe::reject_install)
            {
                return cxx::unexpected(SceneSystemBuildFailure{
                    .code = ESceneSystemBuildError::EXTERNAL_OPERATION_FAILURE,
                    .system = input.instanceId(), .cause = 719});
            }
            auto maintenance =
                builder.addMaintenanceTask<Probe>(input.instanceId(), [](Probe &self) noexcept -> SceneStageResult {
                    ++self.maintenance;
                    return ESceneProgress::COMPLETE;
                });
            if (!maintenance)
            {
                return maintenance;
            }
            auto stable =
                builder.addStablePointTask<Probe>(input.instanceId(), [](Probe &self) noexcept -> SceneStageResult {
                    ++self.stable;
                    if (self.failure)
                    {
                        return cxx::unexpected(SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, 713});
                    }
                    return ESceneProgress::COMPLETE;
                });
            if (!stable)
            {
                return stable;
            }
            return builder.addPublicationTask<Probe>(input.instanceId(), [](Probe &self) noexcept -> SceneStageResult {
                ++self.publication;
                return self.gate ? ESceneProgress::COMPLETE : ESceneProgress::PENDING;
            });
        },
        .projections = projections};
}
} // namespace

int main()
{
    using namespace lux;
    using namespace lux::scene;
    using namespace std::chrono_literals;
    meta::ReflectionRegistry::initRegistry();
    auto meta = SceneMetaManager::build({.scene_systems = {registration()}});
    assert(meta);
    SceneDescriptionBuilder builder;
    assert(builder.addSystem({1}, "probe", registration().type, 1, {}, 0));
    auto invalid_disk = std::move(builder).build();
    assert(!invalid_disk && invalid_disk.error().code == ESceneDescriptionError::INVALID_WORLD);
    auto description = std::move(builder).buildResolved();
    assert(description);
    auto shared = std::make_shared<const SceneDescription>(std::move(*description));
    SceneCreateInfo info{shared,
                         std::make_shared<const world::WorldDescription>(),
                         std::make_shared<const simulation::SimulationDescription>(),
                         *meta,
                         {}};
    auto first = SceneInstance::create(info);
    auto second = SceneInstance::create(info);
    assert(first && second && (*first)->id() != (*second)->id());
    assert((*first)->simulation().seal());
    assert((*second)->simulation().seal());
    Probe::reject_install = true;
    auto rejected = SceneInstance::create(info);
    assert(!rejected && rejected.error().code == ESceneBuildError::SCENE_SYSTEM_BUILD_FAILURE);
    assert(rejected.error().scene_system.code == ESceneSystemBuildError::EXTERNAL_OPERATION_FAILURE);
    assert(std::any_cast<int>(rejected.error().scene_system.cause) == 719);
    assert(Probe::destructions == 1);
    Probe::reject_install = false;
    auto executor = task::TaskExecutor::create({0, 1024});
    assert(executor);
    SceneDriver driver(*executor);
    auto &instance = **first;
    auto &probe = *instance.findSceneSystem<Probe>();
    auto *capability = instance.findSceneSystem<Capability>();
    assert(capability == static_cast<Capability *>(&probe));
    assert(static_cast<void *>(capability) != static_cast<void *>(&probe));
    assert(capability->marker == 42);
    auto now = std::chrono::steady_clock::now();
    assert(driver.play(instance));
    SceneAdvanceBudget budget{32, 1};
    assert(driver.advance(instance, now, budget) == ESceneProgress::PENDING);
    assert(instance.progress().clock.step_index == 1 && probe.stable == 1);
    assert(instance.progress().simulation_completed == 1 && instance.progress().stable_completed == 1);
    assert(instance.progress().publication_completed == 0);
    const auto maintained = probe.maintenance;
    for (int turn = 0; turn < 4; ++turn)
    {
        budget = {32, 1};
        assert(driver.advance(instance, now + 1s, budget) == ESceneProgress::PENDING);
        assert(instance.progress().clock.step_index == 1 && probe.stable == 1);
        assert(budget.new_steps == 1);
    }
    assert(probe.maintenance == maintained + 4);
    assert(driver.pause(instance));
    probe.gate = true;
    budget = {32, 1};
    assert(driver.advance(instance, now + 1s, budget) == ESceneProgress::COMPLETE);
    assert(instance.progress().state == ESceneDriveState::PAUSED);
    assert(instance.progress().publication_completed == 1);
    assert(driver.step(instance));
    auto duplicate = driver.step(instance);
    assert(!duplicate && duplicate.error() == ESceneControlError::BUSY);
    budget = {32, 1};
    assert(driver.advance(instance, now + 2s, budget) == ESceneProgress::COMPLETE);
    assert(instance.progress().clock.step_index == 2 && instance.progress().state == ESceneDriveState::PAUSED);
    driver.invalidate(instance);
    budget = {32, 1};
    assert(driver.advance(instance, now + 2s, budget) == ESceneProgress::COMPLETE);
    assert(instance.progress().clock.step_index == 2 && instance.progress().refresh_completed == 1);
    driver.invalidate(instance);
    budget = {32, 1};
    assert(driver.advance(instance, now + 2s, budget) == ESceneProgress::COMPLETE);
    assert(instance.progress().refresh_completed == 2);
    probe.failure = true;
    assert(driver.step(instance));
    budget = {32, 1};
    static_cast<void>(driver.advance(instance, now + 3s, budget));
    assert(instance.progress().state == ESceneDriveState::FAILED);
    assert(instance.progress().clock.step_index == 3 && instance.progress().simulation_completed == 3);
    assert(instance.progress().stable_completed == 2 && instance.progress().publication_completed == 2);
    const auto &failure = std::get<SceneExecutionFailure>(instance.progress().result.error().cause);
    assert(failure.system.value == 1 && std::any_cast<int>(failure.cause) == 713);
    driver.stop(instance);
    budget = {32, 1};
    static_cast<void>(driver.advance(instance, now + 4s, budget));
    assert(std::any_cast<int>(std::get<SceneExecutionFailure>(instance.progress().result.error().cause).cause) == 713);
    assert((*second)->progress().clock.step_index == 0);
    auto &other = *(*second)->findSceneSystem<Probe>();
    other.gate = false;
    assert(driver.play(**second));
    budget = {32, 1};
    assert(driver.advance(**second, now, budget) == ESceneProgress::PENDING);
    driver.stop(**second);
    budget = {32, 1};
    assert(driver.advance(**second, now, budget) == ESceneProgress::COMPLETE);
    assert((*second)->progress().state == ESceneDriveState::STOPPED);
    first->reset();
    second->reset();
    assert(Probe::destructions == 3);
    std::cout << "PASS SceneDriver: resumed publication, real clock/failure, pause/step/stop, independent instances, "
                 "capability offset, Registry lifetime\n";
}
