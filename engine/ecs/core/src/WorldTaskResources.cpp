#include <lux/engine/ecs/WorldTaskResources.hpp>

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/core/detail/CommandStorage.hpp>
#include <lux/engine/ecs/core/detail/WorldAccess.hpp>
#include <lux/engine/ecs/core/detail/WorldTaskResourceTestAccess.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace lux::ecs
{
    struct WorldChangeBatch::Impl final
    {
        struct Lane final
        {
            std::uint64_t storage{};
            std::vector<Entity> records;
        };

        std::vector<Lane> lanes;
        std::size_t current_records{};
        std::size_t peak_records{};
        std::uint64_t lane_binds{};
        std::uint64_t journal_stream_binds{};
        std::uint64_t record_appends{};
        std::uint64_t per_record_lookups{};
        std::uint64_t history_losses{};
        bool overflow{};
    };

    WorldChangeBatch::WorldChangeBatch()
        : impl_(std::make_unique<Impl>())
    {
    }

    WorldChangeBatch::~WorldChangeBatch() = default;
    WorldChangeBatch::WorldChangeBatch(WorldChangeBatch&&) noexcept = default;
    WorldChangeBatch& WorldChangeBatch::operator=(
        WorldChangeBatch&&
    ) noexcept = default;

    lux::cxx::expected<void, WorldTaskResourceFailure>
    WorldChangeBatch::prepare(
        std::span<const std::uint64_t> write_storages,
        std::size_t reserve_records
    ) noexcept
    {
        try
        {
            impl_->lanes.clear();
            impl_->lanes.reserve(write_storages.size());
            for (const std::uint64_t storage : write_storages)
            {
                if (storage == 0U || std::find(
                        write_storages.begin(),
                        write_storages.begin() + impl_->lanes.size(),
                        storage
                    ) != write_storages.begin() + impl_->lanes.size())
                {
                    return lux::cxx::unexpected(WorldTaskResourceFailure{
                        EWorldTaskResourceError::INVALID_STORAGE
                    });
                }
                Impl::Lane lane;
                lane.storage = storage;
                lane.records.reserve(reserve_records);
                impl_->lanes.push_back(std::move(lane));
            }
            impl_->current_records = 0U;
            impl_->overflow = false;
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected(WorldTaskResourceFailure{
                EWorldTaskResourceError::ALLOCATION_FAILURE
            });
        }
    }

    void WorldChangeBatch::reset() noexcept
    {
        for (auto& lane : impl_->lanes)
            lane.records.clear();
        impl_->current_records = 0U;
        impl_->overflow = false;
    }

    detail::ChangeStreamBinder WorldChangeBatch::binder() noexcept
    {
        return detail::ChangeStreamBinder{
            .context = impl_.get(),
            .bind = [](void* context, std::uint64_t storage) noexcept
            {
                auto& self = *static_cast<Impl*>(context);
                ++self.lane_binds;
                for (auto& lane : self.lanes)
                {
                    if (lane.storage != storage)
                        continue;
                    return detail::BoundWorldChangeStream{
                        .owner = &self,
                        .stream = &lane,
                        .append = [](
                            void* owner,
                            void* stream,
                            Entity entity,
                            EComponentChangeKind
                        ) noexcept -> bool
                        {
                            auto& batch = *static_cast<Impl*>(owner);
                            auto& target = *static_cast<Impl::Lane*>(stream);
                            if (batch.overflow)
                                return false;
                            try
                            {
                                target.records.push_back(entity);
                                ++batch.record_appends;
                                ++batch.current_records;
                                batch.peak_records = std::max(
                                    batch.peak_records,
                                    batch.current_records
                                );
                                return true;
                            }
                            catch (...)
                            {
                                batch.overflow = true;
                                return false;
                            }
                        }
                    };
                }
                self.overflow = true;
                return detail::BoundWorldChangeStream{};
            }
        };
    }

    bool WorldChangeBatch::publish(World& world) noexcept
    {
        if (impl_->overflow)
        {
            detail::markWorldChangeHistoryLoss(world);
            ++impl_->history_losses;
            reset();
            return false;
        }

        const auto bind = detail::worldChangeStreamBinder(world);
        for (const auto& lane : impl_->lanes)
        {
            ++impl_->journal_stream_binds;
            auto stream = bind(lane.storage);
            if (!stream)
            {
                ++impl_->history_losses;
                reset();
                return false;
            }
            for (const Entity entity : lane.records)
            {
                if (!stream(entity, EComponentChangeKind::MODIFIED))
                {
                    ++impl_->history_losses;
                    reset();
                    return false;
                }
            }
        }
        reset();
        return true;
    }

    WorldChangeBatchStats WorldChangeBatch::stats() const noexcept
    {
        WorldChangeBatchStats result;
        result.current_records = impl_->current_records;
        for (const auto& lane : impl_->lanes)
        {
            result.retained_capacity += lane.records.capacity();
        }
        result.peak_records = impl_->peak_records;
        result.lane_binds = impl_->lane_binds;
        result.journal_stream_binds = impl_->journal_stream_binds;
        result.record_appends = impl_->record_appends;
        result.per_record_lookups = impl_->per_record_lookups;
        result.history_losses = impl_->history_losses;
        return result;
    }

    struct WorldCommandBatch::Impl final
    {
        std::vector<detail::CommandShard> producers;
        std::vector<bool> active;
    };

    WorldCommandBatch::WorldCommandBatch()
        : impl_(std::make_unique<Impl>())
    {
    }

    WorldCommandBatch::~WorldCommandBatch() = default;
    WorldCommandBatch::WorldCommandBatch(WorldCommandBatch&&) noexcept = default;
    WorldCommandBatch& WorldCommandBatch::operator=(
        WorldCommandBatch&&
    ) noexcept = default;

    lux::cxx::expected<void, WorldTaskResourceFailure>
    WorldCommandBatch::prepare(
        std::size_t producer_count,
        std::size_t reserve_commands_per_producer
    ) noexcept
    {
        try
        {
            impl_->producers.clear();
            impl_->producers.reserve(producer_count);
            impl_->active.assign(producer_count, false);
            for (std::size_t index{}; index < producer_count; ++index)
            {
                impl_->producers.emplace_back();
                impl_->producers.back().reserve(
                    reserve_commands_per_producer
                );
            }
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected(WorldTaskResourceFailure{
                EWorldTaskResourceError::ALLOCATION_FAILURE
            });
        }
    }

    lux::cxx::expected<
        WorldCommandRecordingScope,
        WorldTaskResourceFailure>
    WorldCommandBatch::begin(std::size_t producer) noexcept
    {
        if (producer >= impl_->producers.size() || impl_->active[producer])
        {
            return lux::cxx::unexpected(WorldTaskResourceFailure{
                EWorldTaskResourceError::INVALID_PRODUCER
            });
        }
        impl_->active[producer] = true;
        return WorldCommandRecordingScope(
            *this,
            producer,
            detail::CommandShardAccess::begin(impl_->producers[producer])
        );
    }

    void WorldCommandBatch::end(std::size_t producer) noexcept
    {
        detail::require(
            producer < impl_->producers.size() && impl_->active[producer]
        );
        detail::CommandShardAccess::end(impl_->producers[producer]);
        impl_->active[producer] = false;
    }

    std::size_t WorldCommandBatch::discarded() const noexcept
    {
        std::size_t result{};
        for (const auto& producer : impl_->producers)
            result += producer.discarded();
        return result;
    }

    std::size_t WorldCommandBatch::allocationEvents() const noexcept
    {
        std::size_t result{};
        for (const auto& producer : impl_->producers)
            result += producer.allocationEvents();
        return result;
    }

    WorldCommandRecordingScope::WorldCommandRecordingScope(
        WorldCommandBatch& owner,
        std::size_t producer,
        WorldCommands commands
    ) noexcept
        : owner_(&owner), producer_(producer), commands_(commands)
    {
    }

    WorldCommandRecordingScope::~WorldCommandRecordingScope() noexcept
    {
        if (owner_ != nullptr)
            owner_->end(producer_);
    }

    WorldCommandRecordingScope::WorldCommandRecordingScope(
        WorldCommandRecordingScope&& other
    ) noexcept
        : owner_(std::exchange(other.owner_, nullptr)),
          producer_(other.producer_),
          commands_(other.commands_)
    {
        other.commands_ = {};
    }

    WorldCommands WorldCommandRecordingScope::commands() const noexcept
    {
        return commands_;
    }

    void applyWorldCommands(
        World& world,
        WorldCommandBatch& commands
    ) noexcept
    {
        for (const bool active : commands.impl_->active)
            detail::require(!active);
        detail::WorldExecutionAccess::beginApplyingCommands(world);
        auto mutation = detail::WorldExecutionAccess::commandMutation(world);
        for (auto& producer : commands.impl_->producers)
            detail::CommandShardAccess::apply(producer, mutation);
        mutation = {};
        detail::WorldExecutionAccess::resume(world);
    }

    void detail::WorldTaskResourceTestAccess::failNextPush(
        WorldCommandBatch& batch,
        std::size_t producer
    ) noexcept
    {
        detail::require(producer < batch.impl_->producers.size());
        batch.impl_->producers[producer].failNextPushForTest();
    }
}
