#include <lux/engine/ecs/HierarchySchema.hpp>
#include <lux/engine/ecs/PersistentEntity.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/TransformSchema.hpp>
#include <lux/engine/ecs/TransformSystem.hpp>
#include <lux/engine/ecs/WorldSection.hpp>
#include <lux/engine/ecs/WorldSnapshot.hpp>

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
        assert(edit.add(std::make_unique<lux::ecs::Transform3DSystem>(hierarchy)));
        assert(edit.add(std::make_unique<lux::ecs::Transform2DSystem>(hierarchy)));
        assert(edit.commit());
        schedule.run(1.0F / 60.0F, 1u);
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
    assert(lux::ecs::setParent(edit, hierarchy, child, root));
    edit = {};

    {
        lux::ecs::Schedule schedule{world};
        auto schedule_edit_result = schedule.edit();
        assert(schedule_edit_result);
        auto schedule_edit = std::move(*schedule_edit_result);
        assert(schedule_edit.add(
            std::make_unique<lux::ecs::Transform3DSystem>(hierarchy)
        ));
        assert(schedule_edit.add(
            std::make_unique<lux::ecs::Transform2DSystem>(hierarchy)
        ));
        assert(schedule_edit.commit());

        schedule.run(1.0F / 60.0F, 1u);
        const auto first_world = world.get<lux::ecs::WorldTransform3D>(child)
            .value.translation();
        assert(near(first_world.x(), 10.0F));
        assert(near(first_world.y(), 2.0F));
        assert(world.find<lux::ecs::WorldTransform2D>(child) != nullptr);

        world.patch<lux::ecs::Transform3D>(root, [](auto& value) noexcept
        {
            value.translation.x() = 20.0F;
        });
        schedule.run(1.0F / 60.0F, 2u);
        const auto moved = world.get<lux::ecs::WorldTransform3D>(child)
            .value.translation();
        assert(near(moved.x(), 20.0F));
        assert(near(moved.y(), 2.0F));

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
        schedule.run(1.0F / 60.0F, 3u);
        schedule.run(1.0F / 60.0F, 4u);
        assert(world.find<lux::ecs::Parent>(child) == nullptr);
        const auto orphan = world.get<lux::ecs::WorldTransform3D>(child)
            .value.translation();
        assert(near(orphan.x(), 0.0F));
        assert(near(orphan.y(), 2.0F));
    }
}
