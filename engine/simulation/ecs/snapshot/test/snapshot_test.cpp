#include <lux/engine/simulation/ecs/EcsSnapshot.hpp>
#include <lux/engine/simulation/ecs/EcsChangeJournal.hpp>
#include <lux/engine/simulation/ecs/SimulationEcsMutation.hpp>
#include <lux/engine/simulation/ecs/core/detail/EcsStateAccess.hpp>

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
        lux::simulation::ecs::Entity target{lux::simulation::ecs::NullEntity};
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
    const auto position_schema = lux::simulation::ecs::makeComponentSchema<Position>(
        lux::simulation::ecs::componentSchemaId("test.position")
    );
    const auto cache_schema = lux::simulation::ecs::makeComponentSchema<DerivedCache>(
        lux::simulation::ecs::componentSchemaId("test.cache"),
        1,
        lux::simulation::ecs::EComponentSnapshotPolicy::REBUILD
    );
    auto schemas = lux::simulation::ecs::ComponentSchemaSet::build(
        {position_schema, cache_schema}
    );
    assert(schemas);
    const std::array snapshot_bindings{
        lux::simulation::ecs::bindComponentSnapshot<Position>(position_schema)
    };
    const lux::simulation::ecs::ComponentSnapshotContribution snapshot_contribution{
        {},
        snapshot_bindings
    };
    auto snapshot_components = lux::simulation::ecs::ComponentSnapshotSet::build(
        *schemas,
        std::span(&snapshot_contribution, 1U)
    );
    assert(snapshot_components);

    lux::simulation::ecs::EcsState source;
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
    std::vector<lux::simulation::ecs::Entity> bulk;
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

    lux::simulation::ecs::detail::ComponentSnapshotTestStats::reset();
    auto snapshot = lux::simulation::ecs::EcsSnapshot::capture(
        source,
        *snapshot_components
    );
    assert(snapshot);
    assert(lux::simulation::ecs::detail::ComponentSnapshotTestStats::clone_calls == 1U);
    assert(
        lux::simulation::ecs::detail::ComponentSnapshotTestStats::storage_lookups == 1U
    );
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

    auto source_edit_result = source.mutate();
    auto source_edit = std::move(*source_edit_result);
    std::vector<lux::simulation::ecs::Entity> source_next;
    source_next.reserve(100);
    for (int index{}; index < 100; ++index)
        source_next.push_back(source_edit.create());
    source_edit = {};

    auto instance_edit_result = (*instance)->mutate();
    auto instance_edit = std::move(*instance_edit_result);
    for (int index{}; index < 100; ++index)
        assert(source_next[index] == instance_edit.create());
    instance_edit = {};

    lux::simulation::ecs::EcsState restored;
    lux::simulation::ecs::EcsChangeJournal restore_journal(
        lux::simulation::ecs::EcsChangeHistoryBudget{4096U, 4096U}
    );
    lux::simulation::ecs::ChangeCursor<Position> restore_cursor;
    assert(
        restore_journal.read(restore_cursor).status() ==
        lux::simulation::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
    auto restored_edit_result = lux::simulation::ecs::beginSimulationEcsMutation(
        restored,
        restore_journal
    );
    auto restored_edit = std::move(*restored_edit_result);
    const auto unrelated = restored_edit.create();
    restored_edit.emplace<DerivedCache>(unrelated, 1);
    restored_edit = {};
    const auto restore_epoch = restore_journal.epoch();
    assert(snapshot->restore(restored));
    assert(restore_journal.epoch() == restore_epoch);
    assert(restored.valid(first));
    assert(restored.get<Position>(third).target == first);
    assert(restored.find<DerivedCache>(first) == nullptr);

    lux::simulation::ecs::EcsState busy;
    {
        auto busy_mutation = busy.mutate();
        assert(busy_mutation);
        const auto busy_restore = snapshot->restore(busy);
        assert(!busy_restore);
        assert(busy_restore.error().code == lux::simulation::ecs::ESnapshotError::WORLD_BUSY);
        *busy_mutation = {};
    }
    assert(snapshot->restore(busy));

    std::atomic_bool wrong_thread_rejected{};
    std::thread wrong_thread(
        [&]
        {
            auto captured = lux::simulation::ecs::EcsSnapshot::capture(
                source,
                *snapshot_components
            );
            wrong_thread_rejected.store(
                !captured &&
                captured.error().code == lux::simulation::ecs::ESnapshotError::WORLD_BUSY,
                std::memory_order_relaxed
            );
        }
    );
    wrong_thread.join();
    assert(wrong_thread_rejected.load(std::memory_order_relaxed));

    lux::simulation::ecs::EcsState invalid;
    auto invalid_edit_result = invalid.mutate();
    auto invalid_edit = std::move(*invalid_edit_result);
    const auto unknown = invalid_edit.create();
    invalid_edit.emplace<UnknownStorage>(unknown, 3);
    invalid_edit = {};
    const auto invalid_snapshot = lux::simulation::ecs::EcsSnapshot::capture(
        invalid,
        *snapshot_components
    );
    assert(!invalid_snapshot);
    assert(invalid_snapshot.error().code ==
        lux::simulation::ecs::ESnapshotError::UNKNOWN_COMPONENT_STORAGE);
}
