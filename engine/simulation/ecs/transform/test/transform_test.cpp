#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/HierarchySystem.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/TransformSystem.hpp>
#include <lux/engine/simulation/ecs/EcsSnapshot.hpp>
#include <lux/engine/simulation/ecs/transform/detail/TransformSystemTestAccess.hpp>
#include <lux/engine/simulation/ecs/task/support/EcsTaskTestRig.hpp>
#include <lux/engine/serialization/external_support/Eigen.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace
{
[[nodiscard]] bool near(float left, float right) noexcept
    {
        return std::abs(left - right) < 0.0001F;
    }

    [[nodiscard]] lux::simulation::ecs::ComponentSchemaSet schemas()
    {
        std::vector<lux::simulation::ecs::ComponentSchema> values{
        };
        const auto hierarchy = lux::simulation::ecs::hierarchyComponentSchemas();
        values.insert(values.end(), hierarchy.begin(), hierarchy.end());
        const auto transform = lux::simulation::ecs::transformComponentSchemas();
        values.insert(values.end(), transform.begin(), transform.end());
        auto built = lux::simulation::ecs::ComponentSchemaSet::build(std::move(values));
        assert(built);
        return *built;
    }

    void installAndResolve(
        lux::simulation::ecs::EcsState& world,
        lux::simulation::ecs::HierarchyIndex& hierarchy
    )
    {
        lux::simulation::ecs::testing::EcsTaskTestRig schedule{world};
        const auto hierarchy_system =
            schedule.add<lux::simulation::ecs::HierarchySystem>(world, hierarchy);
        const auto transform3d =
            schedule.add<lux::simulation::ecs::Transform3DSystem>(hierarchy);
        const auto transform2d =
            schedule.add<lux::simulation::ecs::Transform2DSystem>(hierarchy);
        assert(schedule.compile());
        assert(schedule.run(1.0F / 60.0F, 1u));
    }

    void testQuaternionSerialization()
    {
        std::vector<std::byte> bytes;
        lux::serialization::BinaryWriter writer(bytes);
        for (int index{}; index < 4; ++index)
        {
            assert(writer.writeFloat(0.0F));
        }
        lux::serialization::BinaryReader zero_reader(bytes);
        Eigen::Quaternionf zero;
        const auto zero_result = lux::serialization::read(zero_reader, zero);
        assert(!zero_result);
        assert(zero_result.error().code ==
            lux::serialization::ESerializationError::INVALID_VALUE);

        bytes.clear();
        assert(writer.writeFloat(0.0F));
        assert(writer.writeFloat(0.0F));
        assert(writer.writeFloat(0.0F));
        assert(writer.writeFloat(2.0F));
        lux::serialization::BinaryReader non_unit_reader(bytes);
        Eigen::Quaternionf normalized;
        assert(lux::serialization::read(non_unit_reader, normalized));
        assert(near(normalized.squaredNorm(), 1.0F));
    }
}

int main()
{
    const auto schema_set = schemas();
    lux::simulation::ecs::EcsState world;
    lux::simulation::ecs::HierarchyIndex hierarchy{world};
    auto edit_result = world.mutate();
    assert(edit_result);
    auto edit = std::move(*edit_result);
    const auto root = edit.create();
    const auto child = edit.create();
    edit.emplace<lux::simulation::ecs::Transform3D>(
        root,
        lux::simulation::ecs::Transform3D{
            Eigen::Vector3f{10.0F, 0.0F, 0.0F},
            Eigen::Quaternionf::Identity(),
            Eigen::Vector3f::Ones(),
        }
    );
    edit.emplace<lux::simulation::ecs::Transform3D>(
        child,
        lux::simulation::ecs::Transform3D{
            Eigen::Vector3f{0.0F, 2.0F, 0.0F},
            Eigen::Quaternionf::Identity(),
            Eigen::Vector3f::Ones(),
        }
    );
    edit.emplace<lux::simulation::ecs::Transform2D>(
        child,
        lux::simulation::ecs::Transform2D{
            Eigen::Vector2f{3.0F, 4.0F},
            0.0F,
            Eigen::Vector2f::Ones(),
        }
    );
    assert(lux::simulation::ecs::reparent(edit, child, root));
    for (std::size_t index{}; index < 1000U; ++index)
    {
        const auto unrelated = edit.create();
        edit.emplace<lux::simulation::ecs::Transform3D>(unrelated);
    }
    edit = {};

    {
        lux::simulation::ecs::testing::EcsTaskTestRig schedule{world};
        const auto hierarchy_system =
            schedule.add<lux::simulation::ecs::HierarchySystem>(world, hierarchy);
        const auto transform3d =
            schedule.add<lux::simulation::ecs::Transform3DSystem>(hierarchy);
        const auto transform2d =
            schedule.add<lux::simulation::ecs::Transform2DSystem>(hierarchy);
        assert(schedule.compile());
        auto* transform3d_system = std::addressof(
            schedule.system<lux::simulation::ecs::Transform3DSystem>(transform3d)
        );
        auto* transform2d_system = std::addressof(
            schedule.system<lux::simulation::ecs::Transform2DSystem>(transform2d)
        );

        assert(schedule.run(1.0F / 60.0F, 1u));
        const auto first_world = world.get<lux::simulation::ecs::WorldTransform3D>(child)
            .value.translation();
        assert(near(first_world.x(), 10.0F));
        assert(near(first_world.y(), 2.0F));
        assert(world.find<lux::simulation::ecs::WorldTransform2D>(child) != nullptr);
        assert(
            lux::simulation::ecs::detail::TransformSystemTestAccess::visitedNodes(
                *transform3d_system
            ) == 1002U
        );

        assert(schedule.run(1.0F / 60.0F, 2u));
        assert(
            lux::simulation::ecs::detail::TransformSystemTestAccess::visitedNodes(
                *transform3d_system
            ) == 0U
        );
        assert(
            lux::simulation::ecs::detail::TransformSystemTestAccess::visitedNodes(
                *transform2d_system
            ) == 0U
        );

        auto update_result = schedule.mutate();
        assert(update_result);
        auto update = std::move(*update_result);
        update.update<lux::simulation::ecs::Transform3D>(root, [](auto& value) noexcept
        {
            value.translation.x() = 20.0F;
        });
        update = {};
        assert(schedule.run(1.0F / 60.0F, 3u));
        const auto moved = world.get<lux::simulation::ecs::WorldTransform3D>(child)
            .value.translation();
        assert(near(moved.x(), 20.0F));
        assert(near(moved.y(), 2.0F));
        assert(
            lux::simulation::ecs::detail::TransformSystemTestAccess::visitedNodes(
                *transform3d_system
            ) == 2U
        );

        auto leaf_update_result = schedule.mutate();
        assert(leaf_update_result);
        auto leaf_update = std::move(*leaf_update_result);
        leaf_update.update<lux::simulation::ecs::Transform3D>(
            child,
            [](auto& value) noexcept
            {
                value.translation.y() = 5.0F;
            }
        );
        leaf_update = {};
        assert(schedule.run(1.0F / 60.0F, 4u));
        assert(near(
            world.get<lux::simulation::ecs::WorldTransform3D>(child).value.translation().y(),
            5.0F
        ));
        assert(
            lux::simulation::ecs::detail::TransformSystemTestAccess::visitedNodes(
                *transform3d_system
            ) == 1U
        );

        auto remove_local_result = schedule.mutate();
        assert(remove_local_result);
        auto remove_local = std::move(*remove_local_result);
        remove_local.erase<lux::simulation::ecs::Transform3D>(child);
        remove_local = {};
        assert(schedule.run(1.0F / 60.0F, 5u));
        assert(world.find<lux::simulation::ecs::WorldTransform3D>(child) == nullptr);

        auto restore_local_result = schedule.mutate();
        assert(restore_local_result);
        auto restore_local = std::move(*restore_local_result);
        restore_local.emplace<lux::simulation::ecs::Transform3D>(
            child,
            lux::simulation::ecs::Transform3D{
                Eigen::Vector3f{0.0F, 2.0F, 0.0F},
                Eigen::Quaternionf::Identity(),
                Eigen::Vector3f::Ones(),
            }
        );
        restore_local = {};
        assert(schedule.run(1.0F / 60.0F, 6u));
        assert(near(
            world.get<lux::simulation::ecs::WorldTransform3D>(child).value.translation().x(),
            20.0F
        ));

        auto reparent_result = schedule.mutate();
        assert(reparent_result);
        auto reparent_edit = std::move(*reparent_result);
        const auto alternate_root = reparent_edit.create();
        reparent_edit.emplace<lux::simulation::ecs::Transform3D>(
            alternate_root,
            lux::simulation::ecs::Transform3D{
                Eigen::Vector3f{30.0F, 0.0F, 0.0F},
                Eigen::Quaternionf::Identity(),
                Eigen::Vector3f::Ones(),
            }
        );
        reparent_edit = {};
        assert(schedule.run(1.0F / 60.0F, 7u));

        auto move_branch_result = schedule.mutate();
        assert(move_branch_result);
        auto move_branch = std::move(*move_branch_result);
        assert(lux::simulation::ecs::reparent(move_branch, child, alternate_root));
        move_branch = {};
        assert(schedule.run(1.0F / 60.0F, 8u));
        const auto moved_branch = world.get<lux::simulation::ecs::WorldTransform3D>(child)
            .value.translation();
        assert(near(moved_branch.x(), 30.0F));
        assert(near(moved_branch.y(), 2.0F));
        assert(
            lux::simulation::ecs::detail::TransformSystemTestAccess::visitedNodes(
                *transform3d_system
            ) == 1U
        );

        auto restore_branch_result = schedule.mutate();
        assert(restore_branch_result);
        auto restore_branch = std::move(*restore_branch_result);
        assert(lux::simulation::ecs::reparent(restore_branch, child, root));
        restore_branch = {};
        assert(schedule.run(1.0F / 60.0F, 9u));
        assert(near(
            world.get<lux::simulation::ecs::WorldTransform3D>(child).value.translation().x(),
            20.0F
        ));

        const std::array snapshot_contributions{
            lux::simulation::ecs::hierarchyComponentSnapshotContribution(),
            lux::simulation::ecs::transformComponentSnapshotContribution(),
        };
        auto snapshot_components = lux::simulation::ecs::ComponentSnapshotSet::build(
            schema_set,
            snapshot_contributions
        );
        assert(snapshot_components);
        auto snapshot = lux::simulation::ecs::EcsSnapshot::capture(
            world,
            *snapshot_components
        );
        assert(snapshot);
        auto fork = snapshot->instantiate();
        assert(fork);
        assert((*fork)->find<lux::simulation::ecs::WorldTransform3D>(child) == nullptr);
        lux::simulation::ecs::HierarchyIndex fork_hierarchy{**fork};
        installAndResolve(**fork, fork_hierarchy);
        const auto forked = (*fork)->get<lux::simulation::ecs::WorldTransform3D>(child)
            .value.translation();
        assert(near(forked.x(), 20.0F));
        assert(near(forked.y(), 2.0F));

        auto destroy_result = schedule.mutate();
        assert(destroy_result);
        auto destroy = std::move(*destroy_result);
        destroy.destroy(root);
        destroy = {};
        assert(schedule.run(1.0F / 60.0F, 10u));
        assert(schedule.run(1.0F / 60.0F, 11u));
        assert(world.find<lux::simulation::ecs::Parent>(child) == nullptr);
        const auto orphan = world.get<lux::simulation::ecs::WorldTransform3D>(child)
            .value.translation();
        assert(near(orphan.x(), 0.0F));
        assert(near(orphan.y(), 2.0F));
    }

    testQuaternionSerialization();
}
