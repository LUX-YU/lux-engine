#pragma once

#include <entt/container/dense_map.hpp>
#include <limits>
#include <utility>

#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/script/ScriptWaitSource.hpp>

namespace lux::simulation::script::detail
{
    // Borrowed only while the result exists. Execution owns these identities;
    // EventWaits owns only the embedded routing links below.
    struct ScriptWaitIdentity
    {
        ScriptAwaitableId id;
        ScriptInstanceId instance;
    };

    class ScriptEventWaitLink final
    {
        friend class ScriptEventWaits;
        enum class EState : std::uint8_t { DETACHED, ACTIVE, CLAIMED };
        const ScriptWaitIdentity* wait_{};
        ScriptEventWaitLink* route_previous_{};
        ScriptEventWaitLink* route_next_{};
        ScriptEventWaitLink* instance_previous_{};
        ScriptEventWaitLink* instance_next_{};
        ecs::Entity target_{ecs::NullEntity};
        std::uint32_t endpoint_{};
        EState state_{EState::DETACHED};
    };

    struct ScriptClaimedEventWait final
    {
        ScriptInstanceId instance;
        ScriptAwaitableId awaitable;
        std::uint32_t endpoint{};
    };

    class ScriptEventWaits final
    {
        using Link = ScriptEventWaitLink;
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
                const auto a = std::hash<std::uint32_t>{}(key.bucket_slot);
                const auto b = std::hash<std::uint64_t>{}(ecs::entityBits(key.target));
                return a ^ (b + 0x9e3779b9U + (a << 6U) + (a >> 2U));
            }
        };
        struct EventRouteHead final
        {
            Link* first{};
            Link* last{};
        };
        struct InstanceIndex final
        {
            ScriptInstanceId id;
            Link* first{};
        };
        using EventRouteIndex = entt::dense_map<EventRouteKey, EventRouteHead, EventRouteKeyHash>;

        [[nodiscard]] InstanceIndex* instanceRecord(ScriptInstanceId instance) noexcept
        {
            if (!instance.valid() || instance.slot > instances_.size()) return nullptr;
            auto& result = instances_[instance.slot - 1U];
            return result.id == instance ? &result : nullptr;
        }
        [[nodiscard]] EventRouteHead* findRoute(std::uint32_t endpoint, ecs::Entity target) noexcept
        {
            if (target == ecs::NullEntity) return &broadcast_routes_[endpoint];
            const auto found = routes_.find(EventRouteKey{endpoint, target});
            return found == routes_.end() ? nullptr : &found->second;
        }
        void removeEmptyRoute(std::uint32_t endpoint, ecs::Entity target) noexcept
        {
            if (target == ecs::NullEntity) broadcast_routes_[endpoint] = {};
            else routes_.erase(EventRouteKey{endpoint, target});
        }
        void unlinkRoute(Link& link) noexcept
        {
            auto* route = findRoute(link.endpoint_, link.target_);
            if (route == nullptr) std::terminate();
            if (link.route_previous_) link.route_previous_->route_next_ = link.route_next_;
            else route->first = link.route_next_;
            if (link.route_next_) link.route_next_->route_previous_ = link.route_previous_;
            else route->last = link.route_previous_;
            if (!route->first) removeEmptyRoute(link.endpoint_, link.target_);
        }
        void claimRoute(std::uint32_t endpoint, ecs::Entity target) noexcept
        {
            ++claim_lookups_;
            auto* route = findRoute(endpoint, target);
            if (route == nullptr) return;
            auto* current = route->first;
            // No user code runs here. Detach the complete occurrence batch before
            // callbacks can register new waits, including nested occurrences.
            while (current)
            {
                auto* next = current->route_next_;
                ++dispatch_visits_;
                current->route_previous_ = nullptr;
                current->route_next_ = nullptr;
                current->state_ = Link::EState::CLAIMED;
                ++active_claimed_;
                if (claimed_.size() == claimed_.capacity()) std::terminate();
                claimed_.push_back({current->wait_->instance, current->wait_->id, endpoint});
                current = next;
            }
            removeEmptyRoute(endpoint, target);
        }
    public:
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
                if (owner_) owner_->finishClaim(begin_, end_);
            }
            [[nodiscard]] std::size_t size() const noexcept { return end_ - begin_; }
            [[nodiscard]] ScriptClaimedEventWait at(std::size_t offset) const noexcept
            {
                // Private traversal supplies an in-range offset. The value snapshot
                // survives cancellation, SlotMap reuse and nested dispatch. Execution
                // checks the original wait generation before accessing the result.
                return owner_->claimed_[begin_ + offset];
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
            if (index.first) std::terminate();
            index = {instance, nullptr};
        }
        [[nodiscard]] lux::cxx::expected<Admission, EScriptEventWaitError> reserve(
            ScriptInstanceId instance, std::uint32_t endpoint, ecs::Entity target) noexcept
        {
            auto* owner = instanceRecord(instance);
            if (!owner) return lux::cxx::unexpected(EScriptEventWaitError::INVALID_INSTANCE);
            if (endpoint >= broadcast_routes_.size())
                return lux::cxx::unexpected(EScriptEventWaitError::UNDECLARED_SOURCE);
            const auto reserved = active_ - active_claimed_ + claimed_.size();
            if (reserved >= capacity_)
                return lux::cxx::unexpected(EScriptEventWaitError::WAITER_CAPACITY_EXCEEDED);
            if (sequence_ == std::numeric_limits<std::uint64_t>::max())
                return lux::cxx::unexpected(EScriptEventWaitError::SEQUENCE_EXHAUSTED);
            return Admission{*this, *owner, endpoint, target};
        }
        [[nodiscard]] ScriptSourceId registerWait(
            Admission&& admission, Link& link, const ScriptWaitIdentity& wait) noexcept
        {
            if (std::exchange(admission.owner_, nullptr) != this || link.state_ != Link::EState::DETACHED)
                std::terminate();
            auto& owner = *admission.instance_;
            const auto endpoint = admission.endpoint_;
            const auto target = admission.target_;
            auto& route = target == ecs::NullEntity ? broadcast_routes_[endpoint] :
                routes_.try_emplace(EventRouteKey{endpoint, target}, EventRouteHead{}).first->second;
            ++sequence_;
            link.wait_ = &wait;
            link.endpoint_ = endpoint;
            link.target_ = target;
            link.state_ = Link::EState::ACTIVE;
            link.route_previous_ = route.last;
            if (route.last) route.last->route_next_ = &link;
            else route.first = &link;
            route.last = &link;
            link.instance_next_ = owner.first;
            if (owner.first) owner.first->instance_previous_ = &link;
            owner.first = &link;
            high_water_ = (std::max)(high_water_, ++active_);
            return {wait.id.slot, wait.id.generation};
        }
        [[nodiscard]] ClaimBatch claim(std::uint32_t endpoint, ecs::Entity target) noexcept
        {
            const auto begin = claimed_.size();
            claimRoute(endpoint, target);
            return ClaimBatch{*this, begin, claimed_.size()};
        }
        [[nodiscard]] std::optional<ScriptSourceCancellation> cancel(Link& link) noexcept
        {
            if (link.state_ == Link::EState::DETACHED) return std::nullopt;
            const auto& wait = *link.wait_;
            const ScriptSourceCancellation result{wait.instance, wait.id,
                {{wait.id.slot, wait.id.generation}, EScriptWaitSource::EVENT}};
            if (link.state_ == Link::EState::ACTIVE) unlinkRoute(link);
            else --active_claimed_;
            if (link.instance_previous_) link.instance_previous_->instance_next_ = link.instance_next_;
            else instances_[wait.instance.slot - 1U].first = link.instance_next_;
            if (link.instance_next_) link.instance_next_->instance_previous_ = link.instance_previous_;
            link = {};
            --active_;
            return result;
        }
        [[nodiscard]] std::optional<ScriptSourceCancellation> cancelNext(ScriptInstanceId instance) noexcept
        {
            auto* owner = instanceRecord(instance);
            if (!owner || !owner->first) return std::nullopt;
            ++cleanup_visits_;
            return cancel(*owner->first);
        }
        [[nodiscard]] std::size_t claimedCount() const noexcept { return active_claimed_; }
        void shutdown() noexcept;
        void writeStats(ScriptRuntimeStats& result) const noexcept;
    private:
        void finishClaim(std::size_t begin, std::size_t end) noexcept
        {
            if (claimed_.size() != end) std::terminate();
            claimed_.resize(begin);
        }
        EventRouteIndex routes_;
        std::vector<EventRouteHead> broadcast_routes_;
        std::vector<ScriptClaimedEventWait> claimed_;
        std::vector<InstanceIndex> instances_;
        std::size_t capacity_{};
        std::size_t active_{};
        std::uint64_t sequence_{};
        std::size_t active_claimed_{};
        std::size_t high_water_{};
        std::size_t dispatch_visits_{};
        std::size_t cleanup_visits_{};
        std::uint64_t claim_lookups_{};
    };
} // namespace lux::simulation::script::detail
