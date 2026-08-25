#include <lux/engine/ecs/ChangeSet.hpp>

#include <lux/engine/ecs/World.hpp>

#include <algorithm>
#include <utility>

namespace lux::ecs
{
    lux::cxx::expected<void, SystemFailure> ChangeSet::prepare(
        std::span<const std::uint64_t> write_storages,
        std::size_t reserve_records
    ) noexcept
    {
        try
        {
            lanes_.clear();
            lanes_.reserve(write_storages.size());
            publish_streams_.clear();
            publish_streams_.resize(write_storages.size());
            for (const std::uint64_t storage : write_storages)
            {
                if (std::find(
                    write_storages.begin(),
                    write_storages.begin() + lanes_.size(),
                    storage
                ) != write_storages.begin() + lanes_.size())
                {
                    return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                        .code = ESystemError::INVALID_ACCESS
                    });
                }
                Lane lane;
                lane.storage = storage;
                lane.records.reserve(reserve_records);
                lanes_.push_back(std::move(lane));
            }
            overflow_ = false;
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                .code = ESystemError::ALLOCATION_FAILURE
            });
        }
    }

    void ChangeSet::reset() noexcept
    {
        for (auto& lane : lanes_)
            lane.records.clear();
        overflow_ = false;
    }

    bool ChangeSet::overflowed() const noexcept
    {
        return overflow_;
    }

    std::size_t ChangeSet::recordCount() const noexcept
    {
        std::size_t result{};
        for (const auto& lane : lanes_)
            result += lane.records.size();
        return result;
    }

    std::uint64_t ChangeSet::laneBindCount() const noexcept
    {
        return lane_bind_count_;
    }

    std::uint64_t ChangeSet::journalStreamBindCount() const noexcept
    {
        return journal_stream_bind_count_;
    }

    std::uint64_t ChangeSet::recordAppendCount() const noexcept
    {
        return record_append_count_;
    }

    std::uint64_t ChangeSet::perRecordLookupCount() const noexcept
    {
        return per_record_lookup_count_;
    }

    detail::ChangeStreamBinder ChangeSet::binder() noexcept
    {
        return detail::ChangeStreamBinder{
            .context = this,
            .bind = [](void* context, std::uint64_t storage) noexcept
            {
                auto& self = *static_cast<ChangeSet*>(context);
                ++self.lane_bind_count_;
                for (auto& lane : self.lanes_)
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
                        ) noexcept
                        {
                            auto& change_set = *static_cast<ChangeSet*>(owner);
                            auto& target = *static_cast<Lane*>(stream);
                            if (change_set.overflow_)
                                return;
                            try
                            {
                                target.records.push_back(entity);
                                ++change_set.record_append_count_;
                            }
                            catch (...)
                            {
                                change_set.overflow_ = true;
                            }
                        }
                    };
                }
                self.overflow_ = true;
                return detail::BoundWorldChangeStream{};
            }
        };
    }

    bool ChangeSet::publish(World& world) noexcept
    {
        if (overflow_)
        {
            detail::markWorldChangeHistoryLoss(world);
            reset();
            return false;
        }

        const auto world_binder = detail::worldChangeStreamBinder(world);
        for (std::size_t index{}; index < lanes_.size(); ++index)
        {
            ++journal_stream_bind_count_;
            publish_streams_[index] = world_binder(lanes_[index].storage);
            if (!publish_streams_[index])
            {
                detail::markWorldChangeHistoryLoss(world);
                reset();
                return false;
            }
        }

        for (std::size_t index{}; index < lanes_.size(); ++index)
        {
            const auto stream = publish_streams_[index];
            const auto& lane = lanes_[index];
            for (const Entity entity : lane.records)
                stream(entity, EComponentChangeKind::MODIFIED);
        }
        reset();
        return true;
    }
}
