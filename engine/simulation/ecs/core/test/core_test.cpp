#include <lux/engine/simulation/ecs/EcsChangeJournal.hpp>
#include <lux/engine/simulation/ecs/EcsState.hpp>
#include <lux/engine/simulation/ecs/EcsTaskResources.hpp>
#include <lux/engine/simulation/ecs/SimulationEcsMutation.hpp>
#include <lux/engine/simulation/ecs/core/detail/EcsChangeJournalAccess.hpp>

#include <cassert>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace
{
    struct Position final
    {
        int value{};
    };

    struct AddPosition final
    {
        lux::simulation::ecs::Entity entity{
            lux::simulation::ecs::NullEntity};

        void apply(
            lux::simulation::ecs::SimulationEcsMutation& mutation
        ) noexcept
        {
            mutation.update<Position>(entity, [](Position& value) noexcept
            {
                value.value = 41;
            });
        }
    };
}

int main()
{
    using namespace lux::simulation::ecs;

    EcsState state;
    EcsChangeJournal journal(EcsChangeHistoryBudget{
        256U * 1024U,
        32U * 1024U * 1024U
    });

    auto direct_result = state.mutate();
    assert(direct_result);
    auto direct = std::move(*direct_result);
    const Entity entity = direct.create();
    direct.emplace<Position>(entity, 7);
    direct.update<Position>(entity, [](Position& value) noexcept
    {
        value.value = 11;
    });
    assert(state.get<Position>(entity).value == 11);
    direct = {};

    ChangeCursor<Position> cursor;
    assert(journal.read(cursor).status() == EChangeReadStatus::RESYNC_REQUIRED);

    auto unobserved = state.mutate();
    assert(unobserved);
    unobserved->update<Position>(entity, [](Position& value) noexcept
    {
        value.value = 12;
    });
    *unobserved = {};
    assert(journal.read(cursor).empty());

    auto observed_result = beginSimulationEcsMutation(state, journal);
    assert(observed_result);
    auto observed = std::move(*observed_result);
    observed.update<Position>(entity, [](Position& value) noexcept
    {
        value.value = 13;
    });
    observed = {};
    auto modified = journal.read(cursor);
    assert(modified.status() == EChangeReadStatus::CURRENT);
    assert(modified.size() == 1U);
    assert((*modified.begin()).entity == entity);
    assert((*modified.begin()).kind == EComponentChangeKind::MODIFIED);
    modified = {};

    EcsCommandBatch commands;
    constexpr std::array command_capacities{
        EcsCommandProducerCapacity{1U, 64U}
    };
    assert(commands.prepare(command_capacities));
    {
        auto recording = commands.begin(0U);
        assert(recording);
        auto scope = std::move(*recording);
        assert(
            scope.commands().push(AddPosition{entity}) ==
            ECommandResult::ACCEPTED
        );
    }
    applyEcsCommands(state, journal, commands);
    assert(state.get<Position>(entity).value == 41);
    auto command_change = journal.read(cursor);
    assert(command_change.size() == 1U);
    assert((*command_change.begin()).kind == EComponentChangeKind::MODIFIED);
    command_change = {};

    EcsChangeBatch batch;
    const std::uint64_t position_storage = entt::type_hash<Position>::value();
    assert(batch.prepare(std::span{&position_storage, 1U}, 1U));
    auto batch_stream = batch.binder()(position_storage);
    assert(batch_stream);
    assert(batch_stream(entity, EComponentChangeKind::MODIFIED));
    assert(batch.publish(journal));
    auto batch_change = journal.read(cursor);
    assert(batch_change.size() == 1U);
    const auto batch_stats = batch.stats();
    assert(batch_stats.lane_binds == 1U);
    assert(batch_stats.journal_stream_binds == 1U);
    assert(batch_stats.per_record_lookups == 0U);
    assert(batch_stats.current_records == 0U);

    for (const std::size_t lane_count : {1U, 4U, 16U, 32U})
    {
        std::vector<std::uint64_t> storages(lane_count);
        for (std::size_t index{}; index < lane_count; ++index)
            storages[index] = 100U + index;
        EcsChangeBatch lanes;
        assert(lanes.prepare(storages, 2U));
        for (const std::uint64_t storage : storages)
        {
            auto lane = lanes.binder()(storage);
            assert(lane);
            assert(lane(entity, EComponentChangeKind::MODIFIED));
            assert(lane(entity, EComponentChangeKind::MODIFIED));
        }
        const auto stats = lanes.stats();
        assert(stats.current_records == lane_count * 2U);
        assert(stats.peak_records == lane_count * 2U);
        assert(stats.record_appends == lane_count * 2U);
        assert(stats.lane_binds == lane_count);
        assert(stats.per_record_lookups == 0U);
    }

    EcsChangeJournal failure_journal(EcsChangeHistoryBudget{
        4096U,
        16U * 4096U
    });
    const std::uint64_t initial_epoch = failure_journal.epoch();
    failure_journal.failNextStreamDescriptorForTest();
    detail::EcsChangePublisher publisher(failure_journal);
    assert(!publisher.bindComponent(901U));
    assert(!publisher.bindComponent(902U));
    assert(failure_journal.epoch() == initial_epoch + 1U);

    const std::uint64_t block_epoch = failure_journal.epoch();
    failure_journal.failNextBlockAcquisitionForTest();
    detail::EcsChangePublisher block_publisher(failure_journal);
    auto stream = block_publisher.bindComponent(903U);
    assert(stream);
    assert(!block_publisher.append(
        stream,
        entity,
        EComponentChangeKind::MODIFIED
    ));
    assert(!block_publisher.append(
        stream,
        entity,
        EComponentChangeKind::MODIFIED
    ));
    assert(failure_journal.epoch() == block_epoch + 1U);

    EntityChangeCursor entity_cursor;
    assert(journal.read(entity_cursor).status() == EChangeReadStatus::RESYNC_REQUIRED);
    auto destroy_result = beginSimulationEcsMutation(state, journal);
    assert(destroy_result);
    destroy_result->destroy(entity);
    *destroy_result = {};
    const auto destroyed = journal.read(entity_cursor);
    assert(destroyed.size() == 1U);
    assert((*destroyed.begin()).kind == EEntityChangeKind::DESTROYED);
    assert(!state.valid(entity));
}
