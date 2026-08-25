#include <lux/engine/ecs/ChangeSet.hpp>

#include <lux/engine/ecs/World.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace lux::ecs
{
    struct ChangeSet::Impl final
    {
        struct Lane final
        {
            std::uint64_t storage{};
            std::vector<Entity> records;
        };

        std::vector<Lane> lanes;
        std::uint64_t lane_bind_count{};
        bool overflow{};
    };

    ChangeSet::ChangeSet()
        : impl_(std::make_unique<Impl>())
    {
    }

    ChangeSet::~ChangeSet() = default;
    ChangeSet::ChangeSet(ChangeSet&&) noexcept = default;
    ChangeSet& ChangeSet::operator=(ChangeSet&&) noexcept = default;

    lux::cxx::expected<void, SystemFailure> ChangeSet::prepare(
        std::span<const std::uint64_t> write_storages,
        std::size_t reserve_records
    ) noexcept
    {
        if (!impl_)
        {
            return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                .code = ESystemError::ALLOCATION_FAILURE
            });
        }

        try
        {
            impl_->lanes.clear();
            impl_->lanes.reserve(write_storages.size());
            for (const std::uint64_t storage : write_storages)
            {
                if (std::find(
                    write_storages.begin(),
                    write_storages.begin() + impl_->lanes.size(),
                    storage
                ) != write_storages.begin() + impl_->lanes.size())
                {
                    return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                        .code = ESystemError::INVALID_ACCESS
                    });
                }
                Impl::Lane lane;
                lane.storage = storage;
                lane.records.reserve(reserve_records);
                impl_->lanes.push_back(std::move(lane));
            }
            impl_->overflow = false;
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
        if (!impl_)
            return;
        for (auto& lane : impl_->lanes)
            lane.records.clear();
        impl_->overflow = false;
    }

    bool ChangeSet::overflowed() const noexcept
    {
        return !impl_ || impl_->overflow;
    }

    std::size_t ChangeSet::recordCount() const noexcept
    {
        if (!impl_)
            return 0U;
        std::size_t result{};
        for (const auto& lane : impl_->lanes)
            result += lane.records.size();
        return result;
    }

    std::uint64_t ChangeSet::laneBindCount() const noexcept
    {
        return impl_ ? impl_->lane_bind_count : 0U;
    }

    std::uint64_t ChangeSet::perRecordLookupCount() const noexcept
    {
        return 0U;
    }

    detail::ChangeStreamBinder ChangeSet::binder() noexcept
    {
        return detail::ChangeStreamBinder{
            .context = this,
            .bind = [](void* context, std::uint64_t storage) noexcept
            {
                auto& self = *static_cast<ChangeSet*>(context);
                if (!self.impl_)
                    return detail::BoundWorldChangeStream{};
                ++self.impl_->lane_bind_count;
                for (auto& lane : self.impl_->lanes)
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
                            auto& target = *static_cast<Impl::Lane*>(stream);
                            if (change_set.impl_->overflow)
                                return;
                            try
                            {
                                target.records.push_back(entity);
                            }
                            catch (...)
                            {
                                change_set.impl_->overflow = true;
                            }
                        }
                    };
                }
                self.impl_->overflow = true;
                return detail::BoundWorldChangeStream{};
            }
        };
    }

    bool ChangeSet::publish(World& world) noexcept
    {
        if (!impl_ || impl_->overflow)
        {
            detail::markWorldChangeHistoryLoss(world);
            reset();
            return false;
        }

        const auto world_binder = detail::worldChangeStreamBinder(world);
        for (auto& lane : impl_->lanes)
        {
            const auto stream = world_binder(lane.storage);
            if (!stream)
                continue;
            for (const Entity entity : lane.records)
            {
                stream(entity, EComponentChangeKind::MODIFIED);
            }
        }
        reset();
        return true;
    }
}
