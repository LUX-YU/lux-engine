#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/hierarchy/detail/HierarchyMaintenance.hpp>
#include <lux/engine/simulation/systems/TransformSystem.hpp>
#include <lux/engine/simulation/systems/detail/TransformSystemTestAccess.hpp>

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
}
