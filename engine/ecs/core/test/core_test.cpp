#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/core/detail/ChangeJournal.hpp>

#include <cassert>
#include <cstdint>
#include <utility>

namespace
{
    struct Position final
    {
        int value{};
    };
}

int main()
{
    lux::ecs::World world;

    auto edit_result = world.edit();
    assert(edit_result);
    auto edit = std::move(*edit_result);

    const lux::ecs::Entity first = edit.create();
    edit.emplace<Position>(first, 7);
    assert(world.valid(first));
    assert(world.get<Position>(first).value == 7);

    auto rejected_edit = world.edit();
    assert(!rejected_edit);

    edit.destroy(first);
    assert(!world.valid(first));
    edit = {};

    auto next_edit_result = world.edit();
    assert(next_edit_result);
    auto next_edit = std::move(*next_edit_result);
    const lux::ecs::Entity second = next_edit.create();
    assert(world.valid(second));
    assert(second != first);

    next_edit.emplace<Position>(second, 3);
    lux::ecs::ChangeCursor<Position> position_cursor;
    auto& journal = lux::ecs::detail::WorldChangeAccess::journal(world);
    assert(
        journal.read(position_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
    next_edit.update<Position>(
        second,
        [](Position& position) noexcept
        {
            position.value = 11;
        }
    );
    assert(world.get<Position>(second).value == 11);

    auto modified = journal.read(position_cursor);
    assert(modified.status() == lux::ecs::EChangeReadStatus::CURRENT);
    assert(modified.size() == 1);
    assert((*modified.begin()).entity == second);
    assert(
        (*modified.begin()).kind ==
        lux::ecs::EComponentChangeKind::MODIFIED
    );
    modified = {};

    std::size_t count{};
    for (auto [entity, position] : world.query<lux::ecs::Read<Position>>())
    {
        assert(entity == second);
        assert(position.value == 11);
        ++count;
    }
    assert(count == 1);

    for (auto [entity, position] :
         next_edit.query<lux::ecs::Write<Position>>())
    {
        assert(entity == second);
        position.value = 12;
    }
    auto query_modified = journal.read(position_cursor);
    assert(query_modified.size() == 1);
    assert(
        (*query_modified.begin()).kind ==
        lux::ecs::EComponentChangeKind::MODIFIED
    );
    query_modified = {};

    next_edit.erase<Position>(second);
    auto removed = journal.read(position_cursor);
    assert(removed.size() == 1);
    assert(
        (*removed.begin()).kind ==
        lux::ecs::EComponentChangeKind::REMOVED
    );
    removed = {};

    lux::ecs::EntityChangeCursor entity_cursor;
    assert(
        journal.read(entity_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
    next_edit.destroy(second);
    auto destroyed = journal.read(entity_cursor);
    assert(destroyed.size() == 1);
    assert(
        (*destroyed.begin()).kind ==
        lux::ecs::EEntityChangeKind::DESTROYED
    );

    lux::ecs::World small_world{
        lux::ecs::WorldConfig{
            lux::ecs::ChangeJournalConfig{4096U, 4096U}}};
    auto small_edit_result = small_world.edit();
    assert(small_edit_result);
    auto small_edit = std::move(*small_edit_result);
    const auto small_entity = small_edit.create();
    small_edit.emplace<Position>(small_entity);
    lux::ecs::ChangeCursor<Position> overflow_cursor;
    auto& small_journal =
        lux::ecs::detail::WorldChangeAccess::journal(small_world);
    assert(
        small_journal.read(overflow_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
    for (int index{}; index < 512; ++index)
    {
        small_edit.update<Position>(
            small_entity,
            [](Position& value) noexcept
            {
                ++value.value;
            }
        );
    }
    assert(
        small_journal.read(overflow_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
}
