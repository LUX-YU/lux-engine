#include <lux/engine/ecs/HierarchySchema.hpp>
#include <lux/engine/ecs/HierarchySystem.hpp>
#include <lux/engine/ecs/PersistentEntity.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/TransformSchema.hpp>
#include <lux/engine/ecs/TransformSystem.hpp>
#include <lux/engine/ecs/WorldSnapshot.hpp>
#include <lux/engine/ecs/transform/detail/TransformSystemTestAccess.hpp>
#include <lux/engine/serialization/external_support/Eigen.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]] uuids::uuid uuid(const char* text)
    {
        return uuids::uuid::from_string(text).value();
    }

    [[nodiscard]] bool near(float left, float right) noexcept
    {
        return std::abs(left - right) < 0.0001F;
    }

    [[nodiscard]] lux::ecs::ComponentSchemaSet schemas()
    {
        std::vector<lux::ecs::ComponentSchema> values{
            lux::ecs::persistentIdComponentSchema(),
        };
        const auto hierarchy = lux::ecs::hierarchyComponentSchemas();
        values.insert(values.end(), hierarchy.begin(), hierarchy.end());
        const auto transform = lux::ecs::transformComponentSchemas();
        values.insert(values.end(), transform.begin(), transform.end());
        auto built = lux::ecs::ComponentSchemaSet::build(std::move(values));
        assert(built);
        return *built;
    }

    void installAndResolve(
        lux::ecs::World& world,
        lux::ecs::HierarchyIndex& hierarchy
    )
    {
        lux::ecs::Schedule schedule{world};
        auto edit_result = schedule.edit();
        assert(edit_result);
        auto edit = std::move(*edit_result);
        const auto hierarchy_system = edit.add(
            std::make_unique<lux::ecs::HierarchySystem>(hierarchy),
            lux::ecs::SystemPhase::PreUpdate
        );
        const auto transform3d = edit.add(
            std::make_unique<lux::ecs::Transform3DSystem>(hierarchy)
        );
        const auto transform2d = edit.add(
            std::make_unique<lux::ecs::Transform2DSystem>(hierarchy)
        );
        assert(hierarchy_system && transform3d && transform2d);
        edit.require(transform3d, hierarchy_system);
        edit.require(transform2d, hierarchy_system);
        assert(edit.commit());
        schedule.run(1.0F / 60.0F, 1u);
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
    const auto load_contribution =
        lux::ecs::transformComponentLoadContribution();
    assert(load_contribution.bindings.size() == 2U);

    const auto schema_set = schemas();
    lux::ecs::World world;
    lux::ecs::HierarchyIndex hierarchy{world};
    auto edit_result = world.edit();
    assert(edit_result);
    auto edit = std::move(*edit_result);
    const auto root = edit.create();
    const auto child = edit.create();
    edit.emplace<lux::ecs::PersistentId>(
        root,
        lux::ecs::PersistentEntityId{
            uuid("00000000-0000-4000-8000-000000000001")}
    );
    edit.emplace<lux::ecs::PersistentId>(
        child,
        lux::ecs::PersistentEntityId{
            uuid("00000000-0000-4000-8000-000000000002")}
    );
    edit.emplace<lux::ecs::Transform3D>(
        root,
        lux::ecs::Transform3D{
            Eigen::Vector3f{10.0F, 0.0F, 0.0F},
            Eigen::Quaternionf::Identity(),
            Eigen::Vector3f::Ones(),
        }
    );
    edit.emplace<lux::ecs::Transform3D>(
        child,
        lux::ecs::Transform3D{
            Eigen::Vector3f{0.0F, 2.0F, 0.0F},
            Eigen::Quaternionf::Identity(),
            Eigen::Vector3f::Ones(),
        }
    );
    edit.emplace<lux::ecs::Transform2D>(
        child,
        lux::ecs::Transform2D{
            Eigen::Vector2f{3.0F, 4.0F},
            0.0F,
            Eigen::Vector2f::Ones(),
        }
    );
    assert(lux::ecs::reparent(edit, child, root));
    for (std::size_t index{}; index < 1000U; ++index)
    {
        const auto unrelated = edit.create();
        edit.emplace<lux::ecs::Transform3D>(unrelated);
    }
    edit = {};

    {
        lux::ecs::Schedule schedule{world};
        auto schedule_edit_result = schedule.edit();
        assert(schedule_edit_result);
        auto schedule_edit = std::move(*schedule_edit_result);
        const auto hierarchy_system = schedule_edit.add(
            std::make_unique<lux::ecs::HierarchySystem>(hierarchy),
            lux::ecs::SystemPhase::PreUpdate
        );
        auto transform3d_owner =
            std::make_unique<lux::ecs::Transform3DSystem>(hierarchy);
        auto* transform3d_system = transform3d_owner.get();
        const auto transform3d = schedule_edit.add(
            std::move(transform3d_owner)
        );
        auto transform2d_owner =
            std::make_unique<lux::ecs::Transform2DSystem>(hierarchy);
        auto* transform2d_system = transform2d_owner.get();
        const auto transform2d = schedule_edit.add(
            std::move(transform2d_owner)
        );
        assert(hierarchy_system && transform3d && transform2d);
        schedule_edit.require(transform3d, hierarchy_system);
        schedule_edit.require(transform2d, hierarchy_system);
        assert(schedule_edit.commit());

        schedule.run(1.0F / 60.0F, 1u);
        const auto first_world = world.get<lux::ecs::WorldTransform3D>(child)
            .value.translation();
        assert(near(first_world.x(), 10.0F));
        assert(near(first_world.y(), 2.0F));
        assert(world.find<lux::ecs::WorldTransform2D>(child) != nullptr);
        assert(
            lux::ecs::detail::TransformSystemTestAccess::visitedNodes(
                *transform3d_system
            ) == 1002U
        );

        schedule.run(1.0F / 60.0F, 2u);
        assert(
            lux::ecs::detail::TransformSystemTestAccess::visitedNodes(
                *transform3d_system
            ) == 0U
        );
        assert(
            lux::ecs::detail::TransformSystemTestAccess::visitedNodes(
                *transform2d_system
            ) == 0U
        );

        auto update_result = world.edit();
        assert(update_result);
        auto update = std::move(*update_result);
        update.update<lux::ecs::Transform3D>(root, [](auto& value) noexcept
        {
            value.translation.x() = 20.0F;
        });
        update = {};
        schedule.run(1.0F / 60.0F, 3u);
        const auto moved = world.get<lux::ecs::WorldTransform3D>(child)
            .value.translation();
        assert(near(moved.x(), 20.0F));
        assert(near(moved.y(), 2.0F));
        assert(
            lux::ecs::detail::TransformSystemTestAccess::visitedNodes(
                *transform3d_system
            ) == 2U
        );

        auto leaf_update_result = world.edit();
        assert(leaf_update_result);
        auto leaf_update = std::move(*leaf_update_result);
        leaf_update.update<lux::ecs::Transform3D>(
            child,
            [](auto& value) noexcept
            {
                value.translation.y() = 5.0F;
            }
        );
        leaf_update = {};
        schedule.run(1.0F / 60.0F, 4u);
        assert(near(
            world.get<lux::ecs::WorldTransform3D>(child).value.translation().y(),
            5.0F
        ));
        assert(
            lux::ecs::detail::TransformSystemTestAccess::visitedNodes(
                *transform3d_system
            ) == 1U
        );

        auto remove_local_result = world.edit();
        assert(remove_local_result);
        auto remove_local = std::move(*remove_local_result);
        remove_local.erase<lux::ecs::Transform3D>(child);
        remove_local = {};
        schedule.run(1.0F / 60.0F, 5u);
        assert(world.find<lux::ecs::WorldTransform3D>(child) == nullptr);

        auto restore_local_result = world.edit();
        assert(restore_local_result);
        auto restore_local = std::move(*restore_local_result);
        restore_local.emplace<lux::ecs::Transform3D>(
            child,
            lux::ecs::Transform3D{
                Eigen::Vector3f{0.0F, 2.0F, 0.0F},
                Eigen::Quaternionf::Identity(),
                Eigen::Vector3f::Ones(),
            }
        );
        restore_local = {};
        schedule.run(1.0F / 60.0F, 6u);
        assert(near(
            world.get<lux::ecs::WorldTransform3D>(child).value.translation().x(),
            20.0F
        ));

        auto reparent_result = world.edit();
        assert(reparent_result);
        auto reparent_edit = std::move(*reparent_result);
        const auto alternate_root = reparent_edit.create();
        reparent_edit.emplace<lux::ecs::Transform3D>(
            alternate_root,
            lux::ecs::Transform3D{
                Eigen::Vector3f{30.0F, 0.0F, 0.0F},
                Eigen::Quaternionf::Identity(),
                Eigen::Vector3f::Ones(),
            }
        );
        reparent_edit = {};
        schedule.run(1.0F / 60.0F, 7u);

        auto move_branch_result = world.edit();
        assert(move_branch_result);
        auto move_branch = std::move(*move_branch_result);
        assert(lux::ecs::reparent(move_branch, child, alternate_root));
        move_branch = {};
        schedule.run(1.0F / 60.0F, 8u);
        const auto moved_branch = world.get<lux::ecs::WorldTransform3D>(child)
            .value.translation();
        assert(near(moved_branch.x(), 30.0F));
        assert(near(moved_branch.y(), 2.0F));
        assert(
            lux::ecs::detail::TransformSystemTestAccess::visitedNodes(
                *transform3d_system
            ) == 1U
        );

        auto restore_branch_result = world.edit();
        assert(restore_branch_result);
        auto restore_branch = std::move(*restore_branch_result);
        assert(lux::ecs::reparent(restore_branch, child, root));
        restore_branch = {};
        schedule.run(1.0F / 60.0F, 9u);
        assert(near(
            world.get<lux::ecs::WorldTransform3D>(child).value.translation().x(),
            20.0F
        ));

        const std::array snapshot_contributions{
            lux::ecs::persistentEntityComponentSnapshotContribution(),
            lux::ecs::hierarchyComponentSnapshotContribution(),
            lux::ecs::transformComponentSnapshotContribution(),
        };
        auto snapshot_components = lux::ecs::ComponentSnapshotSet::build(
            schema_set,
            snapshot_contributions
        );
        assert(snapshot_components);
        auto snapshot = lux::ecs::WorldSnapshot::capture(
            world,
            *snapshot_components
        );
        assert(snapshot);
        auto fork = snapshot->instantiate();
        assert(fork);
        assert((*fork)->find<lux::ecs::WorldTransform3D>(child) == nullptr);
        lux::ecs::HierarchyIndex fork_hierarchy{**fork};
        installAndResolve(**fork, fork_hierarchy);
        const auto forked = (*fork)->get<lux::ecs::WorldTransform3D>(child)
            .value.translation();
        assert(near(forked.x(), 20.0F));
        assert(near(forked.y(), 2.0F));

        auto destroy_result = world.edit();
        assert(destroy_result);
        auto destroy = std::move(*destroy_result);
        destroy.destroy(root);
        destroy = {};
        schedule.run(1.0F / 60.0F, 10u);
        schedule.run(1.0F / 60.0F, 11u);
        assert(world.find<lux::ecs::Parent>(child) == nullptr);
        const auto orphan = world.get<lux::ecs::WorldTransform3D>(child)
            .value.translation();
        assert(near(orphan.x(), 0.0F));
        assert(near(orphan.y(), 2.0F));
    }

    testQuaternionSerialization();
}
