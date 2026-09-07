#pragma once

#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/script/ScriptWaitSource.hpp>
#include <lux/cxx/container/SlotMap.hpp>
#include <entt/container/dense_map.hpp>
#include <limits>
#include <utility>

namespace lux::simulation::script::detail
{
    struct ScriptClaimedEventWait final
    {
        ScriptSourceId id;
        ScriptInstanceId instance;
        ScriptAwaitableId awaitable;
        std::uint32_t endpoint{};
    };

    class ScriptEventWaits final
    {
        enum class EEventWaiterState : std::uint8_t
        {
            ACTIVE,
            CLAIMED,
        };

        struct EventRouteKey final
        {
            std::uint32_t bucket_slot{};
            ecs::Entity target{ecs::NullEntity};

            friend bool operator==(EventRouteKey, EventRouteKey) noexcept = default;
        };

        struct EventRouteKeyHash final
        {
            [[nodiscard]] std::size_t operator()(EventRouteKey key) const noexcept
            {
                const auto bucket_hash = std::hash<std::uint32_t>{}(key.bucket_slot);
                const auto entity_hash = std::hash<std::uint64_t>{}(ecs::entityBits(key.target));
                return bucket_hash ^ (entity_hash + 0x9e3779b9U + (bucket_hash << 6U) + (bucket_hash >> 2U));
            }
        };

        struct EventRouteHead final
        {
            ScriptSourceId first;
            ScriptSourceId last;
        };

        struct EventWaiterRecord final
        {
            ScriptSourceId id;
            ScriptInstanceId instance;
            ScriptAwaitableId awaitable;
            std::uint32_t bucket_slot{};
            ecs::Entity target{ecs::NullEntity};
            std::uint64_t sequence{};
            EEventWaiterState state{EEventWaiterState::ACTIVE};
            ScriptSourceId route_previous;
            ScriptSourceId route_next;
            ScriptSourceId instance_previous;
            ScriptSourceId instance_next;
        };


        struct EventWaiterTag;
        using EventWaiterStorage = lux::cxx::SlotMap<EventWaiterRecord, EventWaiterTag>;
        using EventWaiterKey = EventWaiterStorage::key_type;
        using EventRouteIndex = entt::dense_map<EventRouteKey, EventRouteHead, EventRouteKeyHash>;
        struct InstanceIndex final
        {
            ScriptInstanceId id;
            ScriptSourceId first_event_waiter;
        };
        [[nodiscard]] static constexpr ScriptSourceId eventWaiterId(EventWaiterKey key) noexcept
        {
            return {key.index + 1U, key.gen};
        }
        [[nodiscard]] static constexpr EventWaiterKey eventWaiterKey(ScriptSourceId id) noexcept
        {
            return id.valid() ? EventWaiterKey{id.slot - 1U, id.generation} : EventWaiterKey::invalid();
        }
        [[nodiscard]] InstanceIndex* instanceRecord(ScriptInstanceId instance) noexcept
        {
            if (!instance.valid() || instance.slot > instances_.size())
                return nullptr;
            auto& result = instances_[instance.slot - 1U];
            return result.id == instance ? &result : nullptr;
        }
        void unlinkEventWaiterRoute(EventWaiterRecord& waiter) noexcept
        {
            if (waiter.state != EEventWaiterState::ACTIVE)
                return;
            const EventRouteKey key{waiter.bucket_slot, waiter.target};
            auto route = routes_.find(key);
            if (route == routes_.end())
                std::terminate();
            if (waiter.route_previous.valid())
            {
                auto* previous = waiters_.find(eventWaiterKey(waiter.route_previous));
                if (previous == nullptr)
                    std::terminate();
                previous->route_next = waiter.route_next;
            }
            else
            {
                route->second.first = waiter.route_next;
            }
            if (waiter.route_next.valid())
            {
                auto* next = waiters_.find(eventWaiterKey(waiter.route_next));
                if (next == nullptr)
                    std::terminate();
                next->route_previous = waiter.route_previous;
            }
            else
            {
                route->second.last = waiter.route_previous;
            }
            waiter.route_previous = {};
            waiter.route_next = {};
            if (!route->second.first.valid())
                routes_.erase(key);
        }
        void unlinkEventWaiterOwnership(EventWaiterRecord& waiter) noexcept
        {
            auto* owner = instanceRecord(waiter.instance);
            if (waiter.instance_previous.valid())
            {
                auto* previous = waiters_.find(eventWaiterKey(waiter.instance_previous));
                if (previous != nullptr)
                    previous->instance_next = waiter.instance_next;
            }
            else if (owner != nullptr && owner->first_event_waiter == waiter.id)
            {
                owner->first_event_waiter = waiter.instance_next;
            }
            if (waiter.instance_next.valid())
            {
                auto* next = waiters_.find(eventWaiterKey(waiter.instance_next));
                if (next != nullptr)
                    next->instance_previous = waiter.instance_previous;
            }
            waiter.instance_previous = {};
            waiter.instance_next = {};
        }
        void claimEventWaiters(std::uint32_t bucket, ecs::Entity target, std::uint64_t cutoff) noexcept
        {
            ++claim_lookups_;
            auto route = routes_.find(EventRouteKey{bucket, target});
            if (route == routes_.end())
                return;

            auto current = route->second.first;
            while (current.valid())
            {
                auto* waiter = waiters_.find(eventWaiterKey(current));
                if (waiter == nullptr || waiter->state != EEventWaiterState::ACTIVE)
                    std::terminate();
                ++dispatch_visits_;
                if (waiter->sequence > cutoff)
                    break;
                const auto next = waiter->route_next;
                waiter->route_previous = {};
                waiter->route_next = {};
                waiter->state = EEventWaiterState::CLAIMED;
                ++active_claimed_;
                if (claimed_.size() >= claimed_.capacity())
                    std::terminate();
                claimed_.push_back(current);
                current = next;
            }
            if (current.valid())
            {
                route->second.first = current;
                waiters_[eventWaiterKey(current)].route_previous = {};
            }
            else
                routes_.erase(route);
        }

    public:
        class ClaimBatch final
        {
        public:
            ClaimBatch(const ClaimBatch&) = delete;
            ClaimBatch& operator=(const ClaimBatch&) = delete;
            ClaimBatch(ClaimBatch&& other) noexcept
                : owner_(std::exchange(other.owner_, nullptr)), begin_(other.begin_), end_(other.end_) {}
            ~ClaimBatch() noexcept
            {
                if (owner_ != nullptr)
                    owner_->finishClaim(begin_, end_);
            }
            [[nodiscard]] std::size_t size() const noexcept { return end_ - begin_; }
            [[nodiscard]] std::optional<ScriptClaimedEventWait> at(std::size_t offset) const noexcept
            {
                if (owner_ == nullptr || offset >= size())
                    return std::nullopt;
                return owner_->claimedValue(owner_->claimed_[begin_ + offset]);
            }
        private:
            friend class ScriptEventWaits;
            ClaimBatch(ScriptEventWaits& owner, std::size_t begin, std::size_t end) noexcept
                : owner_(&owner), begin_(begin), end_(end) {}
            ScriptEventWaits* owner_{};
            std::size_t begin_{};
            std::size_t end_{};
        };

        void prepare(std::size_t capacity, std::size_t instance_capacity);
        void beginInstance(ScriptInstanceId instance) noexcept
        {
            auto& index = instances_[instance.slot - 1U];
            if (index.first_event_waiter.valid())
                std::terminate();
            index = {instance, {}};
        }
        [[nodiscard]] lux::cxx::expected<void, EScriptEventWaitError> preflight() const noexcept
        {
            const auto reserved = waiters_.size() - active_claimed_ + claimed_.size();
            if (reserved >= capacity_)
                return lux::cxx::unexpected(EScriptEventWaitError::WAITER_CAPACITY_EXCEEDED);
            if (sequence_ == std::numeric_limits<std::uint64_t>::max())
                return lux::cxx::unexpected(EScriptEventWaitError::SEQUENCE_EXHAUSTED);
            return {};
        }
        [[nodiscard]] lux::cxx::expected<ScriptSourceId, EScriptEventWaitError> registerWait(
            ScriptInstanceId instance, ScriptAwaitableId awaitable, std::uint32_t endpoint, ecs::Entity target) noexcept
        {
            auto* owner = instanceRecord(instance);
            if (owner == nullptr)
                return lux::cxx::unexpected(EScriptEventWaitError::INVALID_INSTANCE);
            const auto checked = preflight();
            if (!checked)
                return lux::cxx::unexpected(checked.error());
            const EventRouteKey key{endpoint, target};
            const auto [unused, inserted_route] = routes_.try_emplace(key, EventRouteHead{});
            const auto inserted = waiters_.tryEmplace(EventWaiterRecord{
                {}, instance, awaitable, endpoint, target, ++sequence_, EEventWaiterState::ACTIVE, {}, {}, {}, {}
            });
            if (!inserted)
            {
                if (inserted_route)
                    routes_.erase(key);
                return lux::cxx::unexpected(EScriptEventWaitError::ALLOCATION_FAILURE);
            }
            const auto id = eventWaiterId(*inserted);
            auto& waiter = waiters_[*inserted];
            waiter.id = id;
            auto route = routes_.find(key);
            waiter.route_previous = route->second.last;
            if (waiter.route_previous.valid())
                waiters_[eventWaiterKey(waiter.route_previous)].route_next = id;
            else
                route->second.first = id;
            route->second.last = id;
            waiter.instance_next = owner->first_event_waiter;
            if (waiter.instance_next.valid())
                waiters_[eventWaiterKey(waiter.instance_next)].instance_previous = id;
            owner->first_event_waiter = id;
            high_water_ = (std::max)(high_water_, waiters_.size());
            return id;
        }

        [[nodiscard]] ClaimBatch claim(std::uint32_t endpoint, ecs::Entity target) noexcept
        {
            const auto begin = claimed_.size();
            claimEventWaiters(endpoint, target, sequence_);
            return ClaimBatch{*this, begin, claimed_.size()};
        }

        [[nodiscard]] std::optional<ScriptSourceCancellation> cancel(ScriptSourceId id) noexcept
        {
            auto* waiter = waiters_.find(eventWaiterKey(id));
            if (waiter == nullptr)
                return std::nullopt;
            const ScriptSourceCancellation result{waiter->instance, waiter->awaitable, {id, EScriptWaitSource::EVENT}};
            if (waiter->state == EEventWaiterState::ACTIVE)
                unlinkEventWaiterRoute(*waiter);
            else
                --active_claimed_;
            unlinkEventWaiterOwnership(*waiter);
            static_cast<void>(waiters_.erase(eventWaiterKey(id)));
            return result;
        }
        [[nodiscard]] std::optional<ScriptSourceCancellation> cancelNext(ScriptInstanceId instance) noexcept
        {
            auto* owner = instanceRecord(instance);
            if (owner == nullptr || !owner->first_event_waiter.valid())
                return std::nullopt;
            const auto result = cancel(owner->first_event_waiter);
            if (!result)
                std::terminate();
            ++cleanup_visits_;
            return result;
        }
        [[nodiscard]] std::size_t claimedCount() const noexcept { return active_claimed_; }
        void shutdown() noexcept;
        void writeStats(ScriptRuntimeStats& result) const noexcept;
    private:
        [[nodiscard]] std::optional<ScriptClaimedEventWait> claimedValue(ScriptSourceId id) const noexcept
        {
            const auto* waiter = waiters_.find(eventWaiterKey(id));
            if (waiter == nullptr || waiter->state != EEventWaiterState::CLAIMED)
                return std::nullopt;
            return ScriptClaimedEventWait{id, waiter->instance, waiter->awaitable, waiter->bucket_slot};
        }
        void finishClaim(std::size_t begin, std::size_t end) noexcept
        {
            if (claimed_.size() != end)
                std::terminate();
            claimed_.resize(begin);
        }
        EventWaiterStorage waiters_;
        EventRouteIndex routes_;
        std::vector<ScriptSourceId> claimed_;
        std::vector<InstanceIndex> instances_;
        std::size_t capacity_{};
        std::uint64_t sequence_{};
        std::size_t active_claimed_{};
        std::size_t high_water_{};
        std::size_t dispatch_visits_{};
        std::size_t cleanup_visits_{};
        std::uint64_t claim_lookups_{};
    };
}
