#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/hierarchy/detail/HierarchyMaintenance.hpp>
#include <lux/engine/simulation/TransformSystem.hpp>
#include <lux/engine/simulation/builtin/transform/detail/TransformSystemTestAccess.hpp>
#include <lux/engine/task/TaskExecutor.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <concepts>
#include <type_traits>
#include <utility>

namespace
{
    [[nodiscard]] bool near(double left, double right, double epsilon = 0.000'000'001) noexcept
    {
        return std::abs(left - right) < epsilon;
    }

    struct Fixture final
    {
        static constexpr std::size_t kCapacity = 64U;

        Fixture()
            : maintenance(registry, hierarchy, deltas), transform2d(registry, hierarchy, deltas),
              transform3d(registry, hierarchy, deltas)
        {
            assert(deltas.prepare(kCapacity));
            assert(maintenance.prepare(kCapacity));
            assert(transform2d.prepare(kCapacity));
            assert(transform3d.prepare(kCapacity));
            constexpr std::array capacities{
                lux::simulation::ecs::EcsCommandProducerCapacity{kCapacity * 2U, 16U * 1024U}};
            assert(commands.prepare(capacities));
        }

        void update()
        {
            assert(maintenance.update());
            {
                auto begun = commands.begin(0U);
                assert(begun);
                auto writer = std::move(*begun);
                assert(transform2d.update(writer));
                assert(transform3d.update(writer));
            }
            assert(lux::simulation::ecs::applyEcsCommands(registry, commands));
        }

        lux::simulation::ecs::Registry registry;
        lux::simulation::ecs::HierarchyIndex hierarchy;
        lux::simulation::ecs::HierarchyDeltaBatch deltas;
        lux::simulation::ecs::detail::HierarchyMaintenance maintenance;
        lux::simulation::Transform2DSystem transform2d;
        lux::simulation::Transform3DSystem transform3d;
        lux::simulation::ecs::EcsCommandBuffer commands;
    };

    void testExistingFoldAndReactiveUpdates()
    {
        using namespace lux::simulation::ecs;
        Fixture fixture;
        const Entity root = fixture.registry.create();
        const Entity child = fixture.registry.create();
        fixture.registry.emplace<Transform3D>(
            root,
            Transform3D{Eigen::Vector3d{10.0, 0.0, 0.0}, Eigen::Quaterniond::Identity(), Eigen::Vector3d::Ones()}
        );
        fixture.registry.emplace<Transform3D>(
            child,
            Transform3D{Eigen::Vector3d{0.0, 2.0, 0.0}, Eigen::Quaterniond::Identity(), Eigen::Vector3d::Ones()}
        );
        fixture.registry.emplace<Transform2D>(
            child,
            Transform2D{Eigen::Vector2d{3.0, 4.0}, 0.0, Eigen::Vector2d::Ones()}
        );
        assert(reparent(fixture.registry, child, root));

        fixture.update();
        auto world = fixture.registry.get<WorldTransform3D>(child).value.translation();
        assert(near(world.x(), 10.0));
        assert(near(world.y(), 2.0));
        assert(fixture.registry.all_of<WorldTransform2D>(child));

        fixture.update();
        assert(
            lux::simulation::detail::TransformSystemTestAccess::visitedNodes(fixture.transform2d) == 0U
        );
        assert(
            lux::simulation::detail::TransformSystemTestAccess::visitedNodes(fixture.transform3d) == 0U
        );

        fixture.registry.patch<Transform3D>(root, [](Transform3D& value) noexcept { value.translation.x() = 20.0; });
        fixture.update();
        world = fixture.registry.get<WorldTransform3D>(child).value.translation();
        assert(near(world.x(), 20.0));
        assert(near(world.y(), 2.0));

        fixture.registry.remove<Transform3D>(child);
        fixture.update();
        assert(!fixture.registry.all_of<WorldTransform3D>(child));

        fixture.registry.emplace<Transform3D>(
            child,
            Transform3D{Eigen::Vector3d{0.0, 5.0, 0.0}, Eigen::Quaterniond::Identity(), Eigen::Vector3d::Ones()}
        );
        fixture.update();
        world = fixture.registry.get<WorldTransform3D>(child).value.translation();
        assert(near(world.x(), 20.0));
        assert(near(world.y(), 5.0));

        const Entity alternate = fixture.registry.create();
        fixture.registry.emplace<Transform3D>(
            alternate,
            Transform3D{Eigen::Vector3d{30.0, 0.0, 0.0}, Eigen::Quaterniond::Identity(), Eigen::Vector3d::Ones()}
        );
        fixture.update();
        assert(reparent(fixture.registry, child, alternate));
        fixture.update();
        world = fixture.registry.get<WorldTransform3D>(child).value.translation();
        assert(near(world.x(), 30.0));
        assert(near(world.y(), 5.0));
    }

    void testLargeCoordinateHierarchyPrecision()
    {
        using namespace lux::simulation::ecs;
        Fixture fixture;
        const Entity root = fixture.registry.create();
        const Entity child = fixture.registry.create();
        fixture.registry.emplace<Transform3D>(
            root,
            Transform3D{
                Eigen::Vector3d{1'000'000'000'000.0, -1'000'000'000'000.0, 0.0},
                Eigen::Quaterniond::Identity(),
                Eigen::Vector3d::Ones()
            }
        );
        fixture.registry.emplace<Transform3D>(
            child,
            Transform3D{
                Eigen::Vector3d{0.125, 0.25, 0.5},
                Eigen::Quaterniond::Identity(),
                Eigen::Vector3d::Ones()
            }
        );
        assert(reparent(fixture.registry, child, root));
        fixture.update();

        const auto world = fixture.registry.get<WorldTransform3D>(child).value.translation();
        assert(near(world.x(), 1'000'000'000'000.125, 0.000'001));
        assert(near(world.y(), -999'999'999'999.75, 0.000'001));
        assert(near(world.z(), 0.5));
    }

    void testDirtyOverflowRequiresExplicitResyncCapacity()
    {
        using namespace lux::simulation::ecs;
        Registry registry;
        HierarchyIndex hierarchy;
        HierarchyDeltaBatch deltas;
        detail::HierarchyMaintenance maintenance(registry, hierarchy, deltas);
        lux::simulation::Transform3DSystem transform(registry, hierarchy, deltas);
        EcsCommandBuffer commands;
        assert(deltas.prepare(8U));
        assert(maintenance.prepare(8U));
        assert(transform.prepare(1U));
        constexpr std::array capacities{EcsCommandProducerCapacity{8U, 1024U}};
        assert(commands.prepare(capacities));

        registry.emplace<Transform3D>(registry.create());
        registry.emplace<Transform3D>(registry.create());
        assert(maintenance.update());
        {
            auto begun = commands.begin(0U);
            assert(begun);
            auto writer = std::move(*begun);
            const auto updated = transform.update(writer);
            assert(!updated);
            assert(updated.error() == lux::simulation::ETransformUpdateError::CAPACITY_EXCEEDED);
        }
        commands.discardPending();

        assert(transform.prepare(8U));
        {
            auto begun = commands.begin(0U);
            assert(begun);
            auto writer = std::move(*begun);
            assert(transform.update(writer));
        }
        assert(applyEcsCommands(registry, commands));
        assert(registry.view<const WorldTransform3D>().size() == 2U);
    }

    [[nodiscard]] std::shared_ptr<const lux::simulation::SimulationDescription> registeredDescription(
        std::size_t entity_capacity,
        lux::simulation::ecs::EcsCommandProducerCapacity command_capacity
    )
    {
        using namespace lux::simulation;
        constexpr lux::system::SystemInstanceId Instance{77U};
        const auto configuration = makeTransformSystemConfiguration(entity_capacity, command_capacity);
        assert(configuration);
        SimulationDescriptionBuilder builder;
        assert(builder.addSystem(
            Instance,
            "transform",
            transformSystemDescription(),
            *configuration
        ));
        auto description = std::move(builder).build();
        assert(description);
        return std::make_shared<SimulationDescription>(std::move(*description));
    }

    void testRegisteredCompositionUpdatesBothDimensions()
    {
        using namespace lux::simulation;
        using namespace lux::simulation::ecs;
        SimulationSystemRegistry systems;
        assert(transformSystemRegistrations().size() == 1U);
        assert(systems.add(transformSystemRegistrations()));

        Registry registry;
        const Entity root = registry.create();
        const Entity child = registry.create();
        registry.emplace<Transform2D>(
            root,
            Transform2D{Eigen::Vector2d{10.0, 20.0}, 0.0, Eigen::Vector2d::Ones()}
        );
        registry.emplace<Transform2D>(
            child,
            Transform2D{Eigen::Vector2d{1.0, 2.0}, 0.0, Eigen::Vector2d::Ones()}
        );
        registry.emplace<Transform3D>(
            root,
            Transform3D{Eigen::Vector3d{100.0, 200.0, 300.0}, Eigen::Quaterniond::Identity(), Eigen::Vector3d::Ones()}
        );
        registry.emplace<Transform3D>(
            child,
            Transform3D{Eigen::Vector3d{3.0, 4.0, 5.0}, Eigen::Quaterniond::Identity(), Eigen::Vector3d::Ones()}
        );

        auto simulation = Simulation::create(
            registry,
            registeredDescription(64U, EcsCommandProducerCapacity{128U, 64U * 1024U}),
            systems
        );
        assert(simulation);
        auto executor = lux::task::TaskExecutor::create({0U, 1U});
        assert(executor);
        assert(simulation->execute(*executor));
        assert(reparent(registry, child, root));
        assert(simulation->execute(*executor));

        const auto world2d = registry.get<WorldTransform2D>(child).value.translation();
        const auto world3d = registry.get<WorldTransform3D>(child).value.translation();
        assert(near(world2d.x(), 11.0));
        assert(near(world2d.y(), 22.0));
        assert(near(world3d.x(), 103.0));
        assert(near(world3d.y(), 204.0));
        assert(near(world3d.z(), 305.0));

        assert(simulation->execute(*executor));
    }

    void testRegisteredCompositionDiscardsBothDimensionsOnFailure()
    {
        using namespace lux::simulation;
        using namespace lux::simulation::ecs;
        SimulationSystemRegistry systems;
        assert(systems.add(transformSystemRegistrations()));
        Registry registry;
        const Entity first = registry.create();
        const Entity second = registry.create();
        registry.emplace<Transform2D>(first);
        registry.emplace<Transform3D>(first);
        registry.emplace<Transform3D>(second);

        auto simulation = Simulation::create(
            registry,
            registeredDescription(1U, EcsCommandProducerCapacity{16U, 4096U}),
            systems
        );
        assert(simulation);
        auto executor = lux::task::TaskExecutor::create({0U, 1U});
        assert(executor);
        const auto executed = simulation->execute(*executor);
        assert(!executed);
        assert(!registry.all_of<WorldTransform2D>(first));
        assert(!registry.all_of<WorldTransform3D>(first));
        assert(!registry.all_of<WorldTransform3D>(second));
    }

    void testDoubleMaintenanceWouldLeave3DStale()
    {
        using namespace lux::simulation::ecs;
        Fixture fixture;
        const Entity first_root = fixture.registry.create();
        const Entity second_root = fixture.registry.create();
        const Entity child = fixture.registry.create();
        fixture.registry.emplace<Transform3D>(first_root, Transform3D{
            Eigen::Vector3d{10.0, 0.0, 0.0}, Eigen::Quaterniond::Identity(), Eigen::Vector3d::Ones()
        });
        fixture.registry.emplace<Transform3D>(second_root, Transform3D{
            Eigen::Vector3d{20.0, 0.0, 0.0}, Eigen::Quaterniond::Identity(), Eigen::Vector3d::Ones()
        });
        fixture.registry.emplace<Transform3D>(child, Transform3D{
            Eigen::Vector3d{1.0, 0.0, 0.0}, Eigen::Quaterniond::Identity(), Eigen::Vector3d::Ones()
        });
        fixture.registry.emplace<Transform2D>(child);
        assert(reparent(fixture.registry, child, first_root));
        fixture.update();
        assert(near(fixture.registry.get<WorldTransform3D>(child).value.translation().x(), 11.0));

        assert(reparent(fixture.registry, child, second_root));
        assert(fixture.maintenance.update());
        {
            auto begun = fixture.commands.begin(0U);
            assert(begun);
            auto writer = std::move(*begun);
            assert(fixture.transform2d.update(writer));
            assert(fixture.maintenance.update());
            assert(fixture.transform3d.update(writer));
        }
        assert(applyEcsCommands(fixture.registry, fixture.commands));
        assert(near(fixture.registry.get<WorldTransform3D>(child).value.translation().x(), 11.0));
    }
}

int
main()
{
    static_assert(std::same_as<typename decltype(lux::simulation::ecs::Transform2D{}.translation)::Scalar, double>);
    static_assert(std::same_as<typename decltype(lux::simulation::ecs::Transform3D{}.translation)::Scalar, double>);
    static_assert(std::same_as<typename decltype(lux::simulation::ecs::WorldTransform2D{}.value)::Scalar, double>);
    static_assert(std::same_as<typename decltype(lux::simulation::ecs::WorldTransform3D{}.value)::Scalar, double>);

    testExistingFoldAndReactiveUpdates();
    testLargeCoordinateHierarchyPrecision();
    testDirtyOverflowRequiresExplicitResyncCapacity();
    testRegisteredCompositionUpdatesBothDimensions();
    testRegisteredCompositionDiscardsBothDimensionsOnFailure();
    testDoubleMaintenanceWouldLeave3DStale();
}
