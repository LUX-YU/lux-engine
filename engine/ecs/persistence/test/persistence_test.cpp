#include <lux/engine/ecs/PersistentEntity.hpp>

#include <uuid.h>

#include <array>
#include <cassert>

namespace
{
    [[nodiscard]] lux::ecs::PersistentEntityId persistentId(
        const char* value
    )
    {
        return lux::ecs::PersistentEntityId{
            uuids::uuid::from_string(value).value()};
    }
}

int main()
{
    const auto load_contribution =
        lux::ecs::persistentEntityComponentLoadContribution();
    assert(load_contribution.bindings.size() == 1U);
    assert(
        load_contribution.bindings.front().schema().id ==
        lux::ecs::componentSchemaId("lux.ecs.PersistentId")
    );

    lux::ecs::World world;
    auto edit_result = world.mutate();
    assert(edit_result);
    auto edit = std::move(*edit_result);
    const auto first = edit.create();
    const auto second = edit.create();
    const auto first_id = persistentId(
        "00000000-0000-4000-8000-000000000001"
    );
    const auto second_id = persistentId(
        "00000000-0000-4000-8000-000000000002"
    );
    edit.emplace<lux::ecs::PersistentId>(first, first_id);
    edit.emplace<lux::ecs::PersistentId>(second, second_id);
    edit = {};

    auto index = lux::ecs::PersistentEntityIndex::build(world);
    assert(index);
    assert(index->size() == 2U);
    assert(index->find(first_id) == first);
    assert(index->find(second_id) == second);
    assert(index->find(persistentId(
        "00000000-0000-4000-8000-000000000003"
    )) == lux::ecs::NullEntity);

    auto duplicate_edit_result = world.mutate();
    assert(duplicate_edit_result);
    auto duplicate_edit = std::move(*duplicate_edit_result);
    duplicate_edit.update<lux::ecs::PersistentId>(
        second,
        [first_id](lux::ecs::PersistentId& value) noexcept
        {
            value.value = first_id;
        }
    );
    duplicate_edit = {};
    auto duplicate = lux::ecs::PersistentEntityIndex::build(world);
    assert(!duplicate);
    assert(
        duplicate.error() ==
        lux::ecs::EPersistentEntityIndexError::DUPLICATE_ID
    );

    auto busy_edit_result = world.mutate();
    assert(busy_edit_result);
    auto busy = lux::ecs::PersistentEntityIndex::build(world);
    assert(!busy);
    assert(
        busy.error() == lux::ecs::EPersistentEntityIndexError::WORLD_BUSY
    );
}
