#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/hierarchy/detail/HierarchyMaintenance.hpp>
#include <lux/engine/simulation/systems/TransformSystem.hpp>
#include <lux/engine/simulation/systems/detail/TransformSystemTestAccess.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <utility>

namespace
{
    [[nodiscard]] bool near(float left, float right) noexcept
    {
        return std::abs(left - right) < 0.0001F;
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
            Transform3D{Eigen::Vector3f{10.0F, 0.0F, 0.0F}, Eigen::Quaternionf::Identity(), Eigen::Vector3f::Ones()}
        );
        fixture.registry.emplace<Transform3D>(
            child,
            Transform3D{Eigen::Vector3f{0.0F, 2.0F, 0.0F}, Eigen::Quaternionf::Identity(), Eigen::Vector3f::Ones()}
        );
        fixture.registry.emplace<Transform2D>(
            child,
            Transform2D{Eigen::Vector2f{3.0F, 4.0F}, 0.0F, Eigen::Vector2f::Ones()}
        );
        assert(reparent(fixture.registry, child, root));

        fixture.update();
        auto world = fixture.registry.get<WorldTransform3D>(child).value.translation();
        assert(near(world.x(), 10.0F));
        assert(near(world.y(), 2.0F));
        assert(fixture.registry.all_of<WorldTransform2D>(child));

        fixture.update();
        assert(
            lux::simulation::detail::TransformSystemTestAccess::visitedNodes(fixture.transform2d) == 0U
        );
        assert(
            lux::simulation::detail::TransformSystemTestAccess::visitedNodes(fixture.transform3d) == 0U
        );

        fixture.registry.patch<Transform3D>(root, [](Transform3D& value) noexcept { value.translation.x() = 20.0F; });
        fixture.update();
        world = fixture.registry.get<WorldTransform3D>(child).value.translation();
        assert(near(world.x(), 20.0F));
        assert(near(world.y(), 2.0F));

        fixture.registry.remove<Transform3D>(child);
        fixture.update();
        assert(!fixture.registry.all_of<WorldTransform3D>(child));

        fixture.registry.emplace<Transform3D>(
            child,
            Transform3D{Eigen::Vector3f{0.0F, 5.0F, 0.0F}, Eigen::Quaternionf::Identity(), Eigen::Vector3f::Ones()}
        );
        fixture.update();
        world = fixture.registry.get<WorldTransform3D>(child).value.translation();
        assert(near(world.x(), 20.0F));
        assert(near(world.y(), 5.0F));

        const Entity alternate = fixture.registry.create();
        fixture.registry.emplace<Transform3D>(
            alternate,
            Transform3D{Eigen::Vector3f{30.0F, 0.0F, 0.0F}, Eigen::Quaternionf::Identity(), Eigen::Vector3f::Ones()}
        );
        fixture.update();
        assert(reparent(fixture.registry, child, alternate));
        fixture.update();
        world = fixture.registry.get<WorldTransform3D>(child).value.translation();
        assert(near(world.x(), 30.0F));
        assert(near(world.y(), 5.0F));
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
    testExistingFoldAndReactiveUpdates();
    testDirtyOverflowRequiresExplicitResyncCapacity();
}
