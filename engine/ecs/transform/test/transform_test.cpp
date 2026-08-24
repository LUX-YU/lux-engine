#include <lux/engine/ecs/HierarchySchema.hpp>
#include <lux/engine/ecs/HierarchySystem.hpp>
#include <lux/engine/ecs/PersistentEntity.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/TransformSchema.hpp>
#include <lux/engine/ecs/TransformSystem.hpp>
#include <lux/engine/ecs/WorldSection.hpp>
#include <lux/engine/ecs/WorldSnapshot.hpp>
#include <lux/engine/ecs/transform/detail/TransformSystemTestAccess.hpp>

#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace
{
    class RotationDecodePort final : public lux::ecs::ComponentDecodePort
    {
      public:
        explicit RotationDecodePort(std::array<float, 4> values) noexcept
        {
            for (std::size_t value_index{};
                 value_index < values.size(); ++value_index)
            {
                auto bits = std::bit_cast<std::uint32_t>(values[value_index]);
                for (std::size_t byte_index{}; byte_index < sizeof(float);
                     ++byte_index)
                {
                    bytes_[value_index * sizeof(float) + byte_index] =
                        static_cast<std::byte>(bits & 0xffU);
                    bits >>= 8U;
                }
            }
        }

        bool next(lux::ecs::EncodedPropertyView& property) noexcept override
        {
            if (read_)
                return false;
            read_ = true;
            property = lux::ecs::EncodedPropertyView{
                "rotation",
                lux::ecs::EComponentWireType::FLOATING_POINT,
                bytes_};
            return true;
        }

        lux::cxx::expected<lux::ecs::Entity, lux::ecs::EComponentCodecError>
        resolveEntity(std::span<const std::byte>) const noexcept override
        {
            return lux::cxx::unexpected(
                lux::ecs::EComponentCodecError::INVALID_DATA
            );
        }

        lux::cxx::expected<
            std::array<std::byte, 16>,
            lux::ecs::EComponentCodecError>
        resolveStableReference(
            std::span<const std::byte>
        ) const noexcept override
        {
            return lux::cxx::unexpected(
                lux::ecs::EComponentCodecError::INVALID_DATA
            );
        }

      private:
        std::array<std::byte, 16> bytes_{};
        bool read_{};
    };

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

    void testTransform3DCodec(const lux::ecs::ComponentSchemaSet& schema_set)
    {
        const auto* schema = schema_set.find(
            lux::ecs::componentSchemaId("lux.ecs.Transform3D")
        );
        assert(schema != nullptr);
        assert(schema->codec.decode != nullptr);

        lux::ecs::World world;
        auto edit_result = world.edit();
        assert(edit_result);
        auto edit = std::move(*edit_result);

        const auto zero_entity = edit.create();
        RotationDecodePort zero_rotation{{0.0F, 0.0F, 0.0F, 0.0F}};
        const auto zero_result = schema->codec.decode(
            *schema,
            edit,
            zero_entity,
            schema->version,
            zero_rotation
        );
        assert(!zero_result);
        assert(
            zero_result.error() == lux::ecs::EComponentCodecError::INVALID_DATA
        );
        assert(world.find<lux::ecs::Transform3D>(zero_entity) == nullptr);

        const auto normalized_entity = edit.create();
        RotationDecodePort non_unit_rotation{{0.0F, 0.0F, 0.0F, 2.0F}};
        assert(schema->codec.decode(
            *schema,
            edit,
            normalized_entity,
            schema->version,
            non_unit_rotation
        ));
        const auto& normalized = world.get<lux::ecs::Transform3D>(
            normalized_entity
        );
        assert(near(normalized.rotation.squaredNorm(), 1.0F));
    }
}

int main()
{
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

        auto snapshot = lux::ecs::WorldSnapshot::capture(world, schema_set);
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

        const std::array entities{root, child};
        const std::array selected{
            lux::ecs::componentSchemaId("lux.ecs.Parent"),
            lux::ecs::componentSchemaId("lux.ecs.Transform2D"),
            lux::ecs::componentSchemaId("lux.ecs.Transform3D"),
        };
        auto image = lux::ecs::WorldSectionWriter::build(
            world,
            schema_set,
            lux::ecs::WorldSectionId{
                uuid("10000000-0000-4000-8000-000000000001")},
            lux::ecs::WorldSectionWriteSelection{entities, selected}
        );
        assert(image);
        auto bytes = lux::ecs::encodeWorldSection(*image);
        assert(bytes);
        auto decoded = lux::ecs::decodeWorldSection(*bytes);
        assert(decoded);
        auto loaded = lux::ecs::WorldSectionReader::materialize(
            *decoded,
            schema_set
        );
        assert(loaded);
        auto loaded_index = lux::ecs::PersistentEntityIndex::build(**loaded);
        assert(loaded_index);
        const auto loaded_child = loaded_index->find(
            lux::ecs::PersistentEntityId{
                uuid("00000000-0000-4000-8000-000000000002")}
        );
        lux::ecs::HierarchyIndex loaded_hierarchy{**loaded};
        installAndResolve(**loaded, loaded_hierarchy);
        const auto loaded_world = (*loaded)->get<lux::ecs::WorldTransform3D>(
            loaded_child
        ).value.translation();
        assert(near(loaded_world.x(), 20.0F));
        assert(near(loaded_world.y(), 2.0F));

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

    testTransform3DCodec(schema_set);
}
