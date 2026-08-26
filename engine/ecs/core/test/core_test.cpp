#include <lux/engine/ecs/EcsState.hpp>
#include <lux/engine/ecs/WorldTaskResources.hpp>
#include <lux/engine/ecs/core/detail/EcsStateAccess.hpp>
#include <lux/engine/ecs/core/detail/WorldChangeLog.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

namespace
{
    struct Position final
    {
        int value{};
    };

    struct FirstStream final
    {
        int value{};
    };
}

int main()
{
    lux::ecs::EcsState world{
        lux::ecs::EcsStateConfig{{256U * 1024U, 32U * 1024U * 1024U}}
    };

    struct AddPosition final
    {
        lux::ecs::Entity entity{lux::ecs::NullEntity};

        void apply(lux::ecs::EcsMutation& mutation) noexcept
        {
            mutation.emplace<Position>(entity, 41);
        }
    };

    auto edit_result = world.mutate();
    assert(edit_result);
    auto edit = std::move(*edit_result);

    const lux::ecs::Entity first = edit.create();
    edit.emplace<Position>(first, 7);
    assert(world.valid(first));
    assert(world.get<Position>(first).value == 7);

    auto rejected_edit = world.mutate();
    assert(!rejected_edit);

    edit.destroy(first);
    assert(!world.valid(first));
    edit = {};

    auto next_edit_result = world.mutate();
    assert(next_edit_result);
    auto next_edit = std::move(*next_edit_result);
    const lux::ecs::Entity second = next_edit.create();
    assert(world.valid(second));
    assert(second != first);

    next_edit.emplace<Position>(second, 3);
    lux::ecs::ChangeCursor<Position> position_cursor;
    auto& journal = lux::ecs::detail::WorldChangeAccess::log(world);
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

    const auto stream_binds_before = journal.streamBindCountForTest();
    const auto record_lookups_before = journal.perRecordLookupCountForTest();
    for (auto [entity, position] :
         next_edit.query<lux::ecs::Write<Position>>())
    {
        assert(entity == second);
        position.value = 12;
    }
    assert(journal.streamBindCountForTest() == stream_binds_before + 1U);
    assert(journal.perRecordLookupCountForTest() == record_lookups_before);
    auto query_modified = journal.read(position_cursor);
    assert(query_modified.size() == 1);
    assert(
        (*query_modified.begin()).kind ==
        lux::ecs::EComponentChangeKind::MODIFIED
    );
    query_modified = {};

    auto repeated_query = next_edit.query<lux::ecs::Write<Position>>();
    auto repeated_iterator = repeated_query.begin();
    assert(repeated_iterator != repeated_query.end());
    (void)*repeated_iterator;
    (void)*repeated_iterator;
    auto repeated_modified = journal.read(position_cursor);
    assert(repeated_modified.status() == lux::ecs::EChangeReadStatus::CURRENT);
    assert(repeated_modified.size() == 1U);
    repeated_modified = {};

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

    lux::ecs::EcsState small_world{
        lux::ecs::EcsStateConfig{
            lux::ecs::EcsChangeHistoryBudget{4096U, 4096U}}};
    auto small_edit_result = small_world.mutate();
    assert(small_edit_result);
    auto small_edit = std::move(*small_edit_result);
    const auto small_entity = small_edit.create();
    small_edit.emplace<Position>(small_entity);
    lux::ecs::ChangeCursor<Position> overflow_cursor;
    auto& small_journal =
        lux::ecs::detail::WorldChangeAccess::log(small_world);
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

    small_edit.update<Position>(
        small_entity,
        [](Position& value) noexcept
        {
            ++value.value;
        }
    );
    auto pinned = small_journal.read(overflow_cursor);
    assert(pinned.status() == lux::ecs::EChangeReadStatus::CURRENT);
    assert(pinned.size() == 1U);
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
    assert((*pinned.begin()).entity == small_entity);
    const auto pinned_first = *pinned.begin();
    small_journal.markHistoryLoss();
    assert((*pinned.begin()).entity == pinned_first.entity);
    pinned = {};
    small_edit.update<Position>(
        small_entity,
        [](Position& value) noexcept
        {
            ++value.value;
        }
    );
    assert(
        small_journal.read(overflow_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
    small_edit.update<Position>(
        small_entity,
        [](Position& value) noexcept
        {
            ++value.value;
        }
    );
    const auto recovered = small_journal.read(overflow_cursor);
    assert(recovered.status() == lux::ecs::EChangeReadStatus::CURRENT);
    assert(recovered.size() == 1U);

    lux::ecs::EcsState failure_world{
        lux::ecs::EcsStateConfig{{4096U, 16U * 4096U}}
    };
    auto& failure_journal =
        lux::ecs::detail::WorldChangeAccess::log(failure_world);
    lux::ecs::ChangeCursor<FirstStream> first_stream_cursor;
    assert(
        failure_journal.read(first_stream_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
    failure_journal.failNextStreamDescriptorForTest();
    auto failure_edit_result = failure_world.mutate();
    auto failure_edit = std::move(*failure_edit_result);
    const auto failure_entity = failure_edit.create();
    failure_edit.emplace<FirstStream>(failure_entity, 17);
    assert(failure_world.get<FirstStream>(failure_entity).value == 17);
    assert(
        failure_journal.read(first_stream_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );

    lux::ecs::EntityChangeCursor failed_entity_cursor;
    assert(
        failure_journal.read(failed_entity_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
    failure_journal.establishBaseline();
    assert(
        failure_journal.read(failed_entity_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
    failure_journal.failNextBlockAcquisitionForTest();
    const auto block_failure_entity = failure_edit.create();
    assert(failure_world.valid(block_failure_entity));
    assert(
        failure_journal.read(failed_entity_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
    failure_journal.establishBaseline();
    assert(
        failure_journal.read(failed_entity_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
    failure_journal.failNextBlockAttachForTest();
    const auto attach_failure_entity = failure_edit.create();
    assert(failure_world.valid(attach_failure_entity));
    assert(
        failure_journal.read(failed_entity_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );

    {
        const std::uint64_t epoch = failure_journal.epoch();
        failure_journal.failNextStreamDescriptorForTest();
        lux::ecs::detail::WorldChangePublisher publisher(failure_world);
        assert(!publisher.bindComponent(901U));
        assert(!publisher.bindComponent(902U));
        assert(failure_journal.epoch() == epoch + 1U);
    }
    {
        const std::uint64_t epoch = failure_journal.epoch();
        failure_journal.failNextBlockAcquisitionForTest();
        lux::ecs::detail::WorldChangePublisher publisher(failure_world);
        auto stream = publisher.bindComponent(903U);
        assert(stream);
        assert(!publisher.append(
            stream,
            failure_entity,
            lux::ecs::EComponentChangeKind::MODIFIED
        ));
        assert(!publisher.append(
            stream,
            failure_entity,
            lux::ecs::EComponentChangeKind::MODIFIED
        ));
        assert(failure_journal.epoch() == epoch + 1U);
    }
    {
        const std::uint64_t epoch = failure_journal.epoch();
        failure_journal.failNextBlockAttachForTest();
        lux::ecs::detail::WorldChangePublisher publisher(failure_world);
        auto stream = publisher.bindComponent(904U);
        assert(stream);
        assert(!publisher.append(
            stream,
            failure_entity,
            lux::ecs::EComponentChangeKind::MODIFIED
        ));
        assert(!publisher.appendEntity(
            failure_entity,
            lux::ecs::EEntityChangeKind::ADDED
        ));
        assert(failure_journal.epoch() == epoch + 1U);
    }

    lux::ecs::EcsState task_world{
        lux::ecs::EcsStateConfig{{4096U, 16U * 4096U}}
    };
    auto task_mutation_result = task_world.mutate();
    assert(task_mutation_result);
    auto task_mutation = std::move(*task_mutation_result);
    const auto task_entity = task_mutation.create();
    task_mutation = {};

    lux::ecs::WorldCommandBatch command_batch;
    assert(command_batch.prepare(1U, 1U));
    auto execution_result = task_world.beginTaskExecution();
    assert(execution_result);
    auto execution = std::move(*execution_result);
    {
        auto scope_result = command_batch.begin(0U);
        assert(scope_result);
        auto scope = std::move(*scope_result);
        assert(
            scope.commands().push(AddPosition{task_entity}) ==
            lux::ecs::ECommandResult::ACCEPTED
        );
    }
    lux::ecs::applyWorldCommands(task_world, command_batch);
    assert(task_world.get<Position>(task_entity).value == 41);
    execution = {};

    lux::ecs::WorldChangeBatch change_batch;
    const std::uint64_t position_storage = entt::type_hash<Position>::value();
    assert(change_batch.prepare(std::span{&position_storage, 1U}, 1U));
    auto batch_stream = change_batch.binder()(position_storage);
    assert(batch_stream);
    assert(batch_stream(task_entity, lux::ecs::EComponentChangeKind::MODIFIED));
    const auto pending_batch_stats = change_batch.stats();
    assert(pending_batch_stats.current_records == 1U);
    assert(pending_batch_stats.peak_records == 1U);
    lux::ecs::ChangeCursor<Position> task_cursor;
    auto& task_log = lux::ecs::detail::WorldChangeAccess::log(task_world);
    assert(
        task_log.read(task_cursor).status() ==
        lux::ecs::EChangeReadStatus::RESYNC_REQUIRED
    );
    assert(change_batch.publish(task_world));
    const auto task_changes = task_log.read(task_cursor);
    assert(task_changes.size() == 1U);
    const auto batch_stats = change_batch.stats();
    assert(batch_stats.lane_binds == 1U);
    assert(batch_stats.journal_stream_binds == 1U);
    assert(batch_stats.per_record_lookups == 0U);
    assert(batch_stats.current_records == 0U);
    assert(batch_stats.peak_records == 1U);

    const std::array<std::uint64_t, 4U> four_storages{11U, 12U, 13U, 14U};
    lux::ecs::WorldChangeBatch four_lane_batch;
    assert(four_lane_batch.prepare(four_storages, 2U));
    for (const std::uint64_t storage : four_storages)
    {
        auto stream = four_lane_batch.binder()(storage);
        assert(stream);
        assert(stream(task_entity, lux::ecs::EComponentChangeKind::MODIFIED));
        assert(stream(task_entity, lux::ecs::EComponentChangeKind::MODIFIED));
    }
    const auto four_lane_stats = four_lane_batch.stats();
    assert(four_lane_stats.current_records == 8U);
    assert(four_lane_stats.peak_records == 8U);
    assert(four_lane_stats.record_appends == 8U);
    assert(four_lane_stats.lane_binds == four_storages.size());

    for (const std::size_t lane_count : {16U, 32U})
    {
        std::vector<std::uint64_t> storages(lane_count);
        for (std::size_t index{}; index < lane_count; ++index)
            storages[index] = 100U + index;
        lux::ecs::WorldChangeBatch batch;
        assert(batch.prepare(storages, 2U));
        for (const std::uint64_t storage : storages)
        {
            auto stream = batch.binder()(storage);
            assert(stream);
            assert(stream(
                task_entity,
                lux::ecs::EComponentChangeKind::MODIFIED
            ));
            assert(stream(
                task_entity,
                lux::ecs::EComponentChangeKind::MODIFIED
            ));
        }
        const auto stats = batch.stats();
        assert(stats.current_records == lane_count * 2U);
        assert(stats.peak_records == lane_count * 2U);
        assert(stats.record_appends == lane_count * 2U);
        assert(stats.lane_binds == lane_count);
        assert(stats.per_record_lookups == 0U);
    }
}
