#include <lux/engine/ecs/WorldSnapshot.hpp>
#include <lux/engine/ecs/core/detail/WorldChangeLog.hpp>
#include <lux/engine/ecs/core/detail/WorldAccess.hpp>

#include <atomic>
#include <array>
#include <cassert>
#include <span>
#include <thread>
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
    const std::array snapshot_bindings{
        lux::ecs::bindComponentSnapshot<Position>(position_schema)
    };
    const lux::ecs::ComponentSnapshotContribution snapshot_contribution{
        {},
        snapshot_bindings
    };
    auto snapshot_components = lux::ecs::ComponentSnapshotSet::build(
        *schemas,
        std::span(&snapshot_contribution, 1U)
    );
    assert(snapshot_components);

    lux::ecs::World source;
    auto edit_result = source.mutate();
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

    lux::ecs::detail::ComponentSnapshotTestStats::reset();
    auto snapshot = lux::ecs::WorldSnapshot::capture(
        source,
        *snapshot_components
    );
    assert(snapshot);
    assert(lux::ecs::detail::ComponentSnapshotTestStats::clone_calls == 1U);
    assert(
        lux::ecs::detail::ComponentSnapshotTestStats::storage_lookups == 1U
    );
    auto instance = snapshot->instantiate();
    assert(instance);
    auto& instance_journal = lux::ecs::detail::WorldChangeAccess::log(
        **instance
    );
    assert(instance_journal.recordWriteCountForTest() == 0U);
    assert(instance_journal.dynamicBlockAcquisitionsForTest() == 0U);
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

    const lux::ecs::WorldConfig bounded_config{
        lux::ecs::WorldChangeLogConfig{4096U, 4096U}};
    auto bounded_instance = snapshot->instantiate(bounded_config);
    assert(bounded_instance);
    lux::ecs::ChangeCursor<Position> bounded_cursor;
    auto& bounded_journal = lux::ecs::detail::WorldChangeAccess::log(
        **bounded_instance
    );
    assert(bounded_journal.recordWriteCountForTest() == 0U);
    assert(bounded_journal.dynamicBlockAcquisitionsForTest() == 0U);
    assert(
        bounded_journal.read(bounded_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
    auto bounded_edit_result = (*bounded_instance)->mutate();
    auto bounded_edit = std::move(*bounded_edit_result);
    for (int index{}; index < 512; ++index)
    {
        bounded_edit.update<Position>(
            first,
            [](Position& value) noexcept
            {
                ++value.value;
            }
        );
    }
    bounded_edit = {};
    assert(
        bounded_journal.read(bounded_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );

    auto source_edit_result = source.mutate();
    auto source_edit = std::move(*source_edit_result);
    std::vector<lux::ecs::Entity> source_next;
    source_next.reserve(100);
    for (int index{}; index < 100; ++index)
        source_next.push_back(source_edit.create());
    source_edit = {};

    auto instance_edit_result = (*instance)->mutate();
    auto instance_edit = std::move(*instance_edit_result);
    for (int index{}; index < 100; ++index)
        assert(source_next[index] == instance_edit.create());
    instance_edit = {};

    lux::ecs::World restored{bounded_config};
    lux::ecs::ChangeCursor<Position> restore_cursor;
    auto& restore_journal =
        lux::ecs::detail::WorldChangeAccess::log(restored);
    assert(
        restore_journal.read(restore_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
    auto restored_edit_result = restored.mutate();
    auto restored_edit = std::move(*restored_edit_result);
    const auto unrelated = restored_edit.create();
    restored_edit.emplace<DerivedCache>(unrelated, 1);
    restored_edit = {};
    assert(snapshot->restore(restored));
    assert(
        restore_journal.read(restore_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
    assert(restored.valid(first));
    assert(restored.get<Position>(third).target == first);
    assert(restored.find<DerivedCache>(first) == nullptr);

    lux::ecs::World busy;
    {
        assert(lux::ecs::detail::WorldExecutionAccess::acquire(busy));
        const auto busy_restore = snapshot->restore(busy);
        assert(!busy_restore);
        assert(busy_restore.error().code == lux::ecs::ESnapshotError::WORLD_BUSY);
        lux::ecs::detail::WorldExecutionAccess::release(busy);
    }
    assert(snapshot->restore(busy));

    std::atomic_bool wrong_thread_rejected{};
    std::thread wrong_thread(
        [&]
        {
            auto captured = lux::ecs::WorldSnapshot::capture(
                source,
                *snapshot_components
            );
            wrong_thread_rejected.store(
                !captured &&
                captured.error().code == lux::ecs::ESnapshotError::WORLD_BUSY,
                std::memory_order_relaxed
            );
        }
    );
    wrong_thread.join();
    assert(wrong_thread_rejected.load(std::memory_order_relaxed));

    lux::ecs::World invalid;
    auto invalid_edit_result = invalid.mutate();
    auto invalid_edit = std::move(*invalid_edit_result);
    const auto unknown = invalid_edit.create();
    invalid_edit.emplace<UnknownStorage>(unknown, 3);
    invalid_edit = {};
    const auto invalid_snapshot = lux::ecs::WorldSnapshot::capture(
        invalid,
        *snapshot_components
    );
    assert(!invalid_snapshot);
    assert(invalid_snapshot.error().code ==
        lux::ecs::ESnapshotError::UNKNOWN_COMPONENT_STORAGE);
}
