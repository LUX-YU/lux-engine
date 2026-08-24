#include <lux/engine/ecs/WorldSnapshot.hpp>
#include <lux/engine/ecs/Schedule.hpp>

#include <cassert>
#include <utility>
#include <vector>

namespace
{
    struct Position final
    {
        int value{};
        lux::ecs::Entity target{lux::ecs::NullEntity};
    };

    struct DerivedCache final
    {
        int value{};
    };

    struct UnknownStorage final
    {
        int value{};
    };
}

int main()
{
    const auto position_schema = lux::ecs::makeComponentSchema<Position>(
        lux::ecs::componentSchemaId("test.position")
    );
    const auto cache_schema = lux::ecs::makeComponentSchema<DerivedCache>(
        lux::ecs::componentSchemaId("test.cache"),
        1,
        lux::ecs::EComponentSnapshotPolicy::REBUILD
    );
    auto schemas = lux::ecs::ComponentSchemaSet::build(
        {position_schema, cache_schema}
    );
    assert(schemas);

    lux::ecs::World source;
    auto edit_result = source.edit();
    assert(edit_result);
    auto edit = std::move(*edit_result);
    const auto first = edit.create();
    const auto removed = edit.create();
    const auto third = edit.create();
    edit.emplace<Position>(first, 7, third);
    edit.emplace<Position>(third, 9, first);
    edit.emplace<DerivedCache>(first, 99);
    edit.destroy(removed);
    std::vector<lux::ecs::Entity> bulk;
    bulk.reserve(10'000);
    for (int index{}; index < 10'000; ++index)
    {
        const auto entity = edit.create();
        edit.emplace<Position>(entity, index, first);
        bulk.push_back(entity);
    }
    for (std::size_t index{}; index < bulk.size(); index += 3)
        edit.destroy(bulk[index]);
    edit = {};

    auto snapshot = lux::ecs::WorldSnapshot::capture(source, *schemas);
    assert(snapshot);
    auto instance = snapshot->instantiate();
    assert(instance);
    assert((*instance)->valid(first));
    assert((*instance)->valid(third));
    assert(!(*instance)->valid(removed));
    assert((*instance)->get<Position>(first).target == third);
    assert((*instance)->find<DerivedCache>(first) == nullptr);
    for (std::size_t index{}; index < bulk.size(); ++index)
    {
        assert((*instance)->valid(bulk[index]) == (index % 3 != 0));
        if (index % 3 != 0)
            assert((*instance)->get<Position>(bulk[index]).value == index);
    }

    auto source_edit_result = source.edit();
    auto source_edit = std::move(*source_edit_result);
    std::vector<lux::ecs::Entity> source_next;
    source_next.reserve(100);
    for (int index{}; index < 100; ++index)
        source_next.push_back(source_edit.create());
    source_edit = {};

    auto instance_edit_result = (*instance)->edit();
    auto instance_edit = std::move(*instance_edit_result);
    for (int index{}; index < 100; ++index)
        assert(source_next[index] == instance_edit.create());
    instance_edit = {};

    lux::ecs::World restored;
    auto restored_edit_result = restored.edit();
    auto restored_edit = std::move(*restored_edit_result);
    const auto unrelated = restored_edit.create();
    restored_edit.emplace<DerivedCache>(unrelated, 1);
    restored_edit = {};
    assert(snapshot->restore(restored));
    assert(restored.valid(first));
    assert(restored.get<Position>(third).target == first);
    assert(restored.find<DerivedCache>(first) == nullptr);

    lux::ecs::World busy;
    {
        lux::ecs::Schedule live_schedule(busy);
        const auto busy_restore = snapshot->restore(busy);
        assert(!busy_restore);
        assert(busy_restore.error().code == lux::ecs::ESnapshotError::WORLD_BUSY);
    }
    assert(snapshot->restore(busy));

    lux::ecs::World invalid;
    auto invalid_edit_result = invalid.edit();
    auto invalid_edit = std::move(*invalid_edit_result);
    const auto unknown = invalid_edit.create();
    invalid_edit.emplace<UnknownStorage>(unknown, 3);
    invalid_edit = {};
    const auto invalid_snapshot = lux::ecs::WorldSnapshot::capture(
        invalid,
        *schemas
    );
    assert(!invalid_snapshot);
    assert(invalid_snapshot.error().code ==
        lux::ecs::ESnapshotError::UNKNOWN_COMPONENT_STORAGE);
}
