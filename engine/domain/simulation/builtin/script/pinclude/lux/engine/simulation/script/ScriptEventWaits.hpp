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
        [[nodiscard]] EventRouteHead* findRoute(std::uint32_t endpoint, ecs::Entity target) noexcept
        {
            if (target == ecs::NullEntity)
                return &broadcast_routes_[endpoint];
            const auto route = routes_.find(EventRouteKey{endpoint, target});
            return route == routes_.end() ? nullptr : &route->second;
        }
        void removeEmptyRoute(std::uint32_t endpoint, ecs::Entity target) noexcept
        {
            if (target == ecs::NullEntity)
                broadcast_routes_[endpoint] = {};
            else
                routes_.erase(EventRouteKey{endpoint, target});
        }
        void unlinkEventWaiterRoute(EventWaiterRecord& waiter) noexcept
        {
            if (waiter.state != EEventWaiterState::ACTIVE)
                return;
            auto* route = findRoute(waiter.bucket_slot, waiter.target);
            if (route == nullptr)
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
                route->first = waiter.route_next;
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
                route->last = waiter.route_previous;
            }
            waiter.route_previous = {};
            waiter.route_next = {};
            if (!route->first.valid())
                removeEmptyRoute(waiter.bucket_slot, waiter.target);
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
            auto* route = findRoute(bucket, target);
            if (route == nullptr)
                return;

            auto current = route->first;
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
                route->first = current;
                waiters_[eventWaiterKey(current)].route_previous = {};
            }
            else
                removeEmptyRoute(bucket, target);
        }

    public:
        // A synchronous owner-thread preflight. Between this and commit only result-storage admission
        // may run: no user code, waiter mutation or region exit. No capacity is consumed before commit.
        class Admission final
        {
        public:
            Admission(const Admission&) = delete;
            Admission& operator=(const Admission&) = delete;
            Admission(Admission&& other) noexcept
                : owner_(std::exchange(other.owner_, nullptr)), instance_(other.instance_),
                  endpoint_(other.endpoint_), target_(other.target_) {}
        private:
            friend class ScriptEventWaits;
            Admission(ScriptEventWaits& owner, InstanceIndex& instance, std::uint32_t endpoint,
                ecs::Entity target) noexcept
                : owner_(&owner), instance_(&instance), endpoint_(endpoint), target_(target) {}
            ScriptEventWaits* owner_{};
            InstanceIndex* instance_{};
            std::uint32_t endpoint_{};
            ecs::Entity target_{ecs::NullEntity};
        };

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

        void prepare(std::size_t capacity, std::size_t instance_capacity, std::size_t endpoint_count);
        void beginInstance(ScriptInstanceId instance) noexcept
        {
            auto& index = instances_[instance.slot - 1U];
            if (index.first_event_waiter.valid())
                std::terminate();
            index = {instance, {}};
        }
        [[nodiscard]] lux::cxx::expected<Admission, EScriptEventWaitError> reserve(
            ScriptInstanceId instance, std::uint32_t endpoint, ecs::Entity target) noexcept
        {
            auto* owner = instanceRecord(instance);
            if (owner == nullptr)
                return lux::cxx::unexpected(EScriptEventWaitError::INVALID_INSTANCE);
            if (endpoint >= broadcast_routes_.size())
                return lux::cxx::unexpected(EScriptEventWaitError::UNDECLARED_SOURCE);
            const auto reserved = waiters_.size() - active_claimed_ + claimed_.size();
            if (reserved >= capacity_)
                return lux::cxx::unexpected(EScriptEventWaitError::WAITER_CAPACITY_EXCEEDED);
            if (sequence_ == std::numeric_limits<std::uint64_t>::max())
                return lux::cxx::unexpected(EScriptEventWaitError::SEQUENCE_EXHAUSTED);
            return Admission{*this, *owner, endpoint, target};
        }
        [[nodiscard]] lux::cxx::expected<ScriptSourceId, EScriptEventWaitError> registerWait(
            Admission&& admission, ScriptAwaitableId awaitable) noexcept
        {
            if (std::exchange(admission.owner_, nullptr) != this)
                std::terminate();
            auto* owner = admission.instance_;
            const auto instance = owner->id;
            const auto endpoint = admission.endpoint_;
            const auto target = admission.target_;
            const EventRouteKey key{endpoint, target};
            EventRouteHead* route{};
            bool inserted_route{};
            if (target == ecs::NullEntity)
                route = &broadcast_routes_[endpoint];
            else
            {
                const auto inserted = routes_.try_emplace(key, EventRouteHead{});
                route = &inserted.first->second;
                inserted_route = inserted.second;
            }
            const auto inserted = waiters_.tryEmplace(EventWaiterRecord{
                {}, instance, awaitable, endpoint, target, sequence_ + 1U, EEventWaiterState::ACTIVE, {}, {}, {}, {}
            });
            if (!inserted)
            {
                if (inserted_route)
                    routes_.erase(key);
                return lux::cxx::unexpected(EScriptEventWaitError::ALLOCATION_FAILURE);
            }
            ++sequence_;
            const auto id = eventWaiterId(*inserted);
            auto& waiter = waiters_[*inserted];
            waiter.id = id;
            waiter.route_previous = route->last;
            if (waiter.route_previous.valid())
                waiters_[eventWaiterKey(waiter.route_previous)].route_next = id;
            else
                route->first = id;
            route->last = id;
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
        std::vector<EventRouteHead> broadcast_routes_;
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
