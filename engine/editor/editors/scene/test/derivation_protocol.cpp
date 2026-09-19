#include <cassert>
#include <cstdio>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/TransformSystem.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <thread>

namespace
{
    using namespace lux;
    struct Marker
    {
        int value{};
    };
    struct Evolution final
    {
        inline static constexpr auto Access = simulation::makeSystemAccessSpec<simulation::ComponentWrite<Marker>>();
        inline static constexpr std::array Hooks{
            simulation::makeHookPointSpec<void()>(simulation::HookPointId{1}, "commit", true, true)};
        inline static constexpr simulation::SimulationSystemDescription Description{
            .type = {.canonical_name = "test.main.evolution", .version = 1}, .hooks = Hooks};
        simulation::ecs::Registry &registry;
        simulation::SimulationCommandProducer producer;
        unsigned steps{}, hooks{};
        std::thread::id owner{std::this_thread::get_id()};
        bool advance() noexcept
        {
            assert(std::this_thread::get_id() == owner);
            ++steps;
            return true;
        }
    };
    Evolution *installed{};
    unsigned commits{};
    bool committed(void *, const simulation::SimulationClockSnapshot &) noexcept
    {
        ++commits;
        if (commits == 1)
        {
            // The final Hook has already flushed its prefix. This follow-up
            // belongs to the next evolution step, not to a paused refresh.
            auto writer = installed->producer.begin();
            assert(writer);
            auto entity = installed->registry.view<Marker>().front();
            assert(writer->replace<Marker>(entity, Marker{99}));
        }
        return true;
    }
} // namespace

int main()
{
    using namespace lux;
    using namespace std::chrono_literals;
    meta::ReflectionRegistry::initRegistry();
    // Installs the existing generated Transform configuration reflection.
    auto editor_metadata = editor::scene::sceneMetadata({});
    assert(editor_metadata);
    simulation::ecs::Registry registry;
    auto entity = registry.create();
    registry.emplace<Marker>(entity);
    registry.emplace<simulation::ecs::Transform3D>(entity);
    simulation::SimulationSystemRegistry systems;
    assert(systems.add(simulation::transformSystemRegistrations()));
    const simulation::SimulationSystemRegistration registration{
        .type = system::systemTypeId(Evolution::Description.type.canonical_name),
        .cpp_type = cxx::typeToken<Evolution>(),
        .description = &Evolution::Description,
        .access = Evolution::Access.spec(),
        .install = [](simulation::SimulationBuilder &builder, simulation::SimulationSystemView description) noexcept
            -> cxx::expected<void, simulation::SimulationSystemBuildFailure>
        {
            auto producer = builder.prepareCommandProducer(
                simulation::SimulationExecutionPoint::hook(description.instanceId(), simulation::HookPointId{1}),
                {8, 1024});
            if (!producer)
            {
                return cxx::unexpected(producer.error());
            }
            auto system = builder.emplaceSystem<Evolution>(description.instanceId(), builder.registry(), *producer);
            if (!system)
            {
                return cxx::unexpected(system.error());
            }
            installed = *system;
            auto task = builder.addSystemTask<Evolution>(description.instanceId(),
                                                         [](auto &value) noexcept { return value.advance(); });
            if (!task)
            {
                return task;
            }
            return builder.addSystemHookTask<Evolution>(description.instanceId(), simulation::HookPointId{1},
                                                        [](auto &value, const auto &) noexcept { ++value.hooks; });
        }};
    assert(systems.add(registration));
    simulation::SimulationDescriptionBuilder description;
    auto config = simulation::makeTransformSystemConfiguration(32, {128, 32768});
    assert(config);
    assert(description.addSystem(system::SystemInstanceId{1}, "evolution", Evolution::Description));
    assert(description.addSystem(system::SystemInstanceId{2}, "transform", simulation::transformSystemDescription(),
                                 *config));
    assert(description.addExecutionDependency(simulation::SimulationExecutionPoint::task(system::SystemInstanceId{1}),
                                              simulation::SimulationExecutionPoint::task(system::SystemInstanceId{2})));
    assert(description.addExecutionDependency(
        simulation::SimulationExecutionPoint::task(system::SystemInstanceId{2}),
        simulation::SimulationExecutionPoint::hook(system::SystemInstanceId{1}, simulation::HookPointId{1})));
    auto built = std::move(description).build();
    assert(built);
    auto simulation = simulation::Simulation::create(
        registry, std::make_shared<const simulation::SimulationDescription>(std::move(*built)), systems);
    if (!simulation)
    {
        std::fprintf(stderr, "Simulation build failure code=%u system=%llu\n", unsigned(simulation.error().code),
                     static_cast<unsigned long long>(simulation.error().system.value));
    }
    assert(simulation);
    auto connection = simulation->bindHookCallbacks({.context = &commits,
                                                     .before = [](void *, const auto &, bool) noexcept { return true; },
                                                     .after = [](void *, const auto &, bool) noexcept { return true; },
                                                     .committed = &committed});
    assert(connection && simulation->seal());
    auto executor = task::TaskExecutor::create({0, 64});
    assert(executor && simulation->execute(*executor, 10ms));
    assert(installed->steps == 1 && installed->hooks == 1 && commits == 1);
    assert(registry.get<Marker>(entity).value == 0);
    for (unsigned i = 1; i <= 3; ++i)
    {
        registry.patch<simulation::ecs::Transform3D>(entity, [i](auto &value) { value.translation.x() = i * 3.0; });
        assert(simulation->refresh(*executor));
        assert(simulation->clock().snapshot().step_index == 1 && simulation->clock().snapshot().elapsed == 10ms);
        assert(installed->steps == 1 && installed->hooks == 1 && commits == 1);
        assert(registry.get<Marker>(entity).value == 0);
        assert(registry.get<simulation::ecs::WorldTransform3D>(entity).value.translation().x() == i * 3.0);
    }
    assert(simulation->execute(*executor, 10ms));
    assert(registry.get<Marker>(entity).value == 99);
    assert(installed->steps == 2 && installed->hooks == 2 && commits == 2);
    connection->reset();
    simulation->stop();
    std::puts("PASS derivation: same systems, no clock/evolution/Hook advance, real Transform update, pending "
              "evolution command preserved");
}
