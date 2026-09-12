#pragma once

#include <entt/container/dense_map.hpp>
#include <array>
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
        ScriptAwaitableId awaitable_;
        std::uint32_t page_{};
        std::uint8_t position_{};
        ecs::Entity target_{ecs::NullEntity};
        std::uint32_t endpoint_{};
        EState state_{EState::DETACHED};
    };

    struct ScriptClaimedEventWait final
    {
        ScriptAwaitableId awaitable;
        std::uint32_t endpoint{};
    };

    class ScriptEventWaits final
    {
        using Link = ScriptEventWaitLink;
        static constexpr std::uint32_t NoPage = (std::numeric_limits<std::uint32_t>::max)();
        static constexpr std::size_t PageSize = 4U;
        struct WaitPage final
        {
            std::array<Link*, PageSize> entries;
            std::uint32_t previous{NoPage};
            std::uint32_t next{NoPage};
            std::uint32_t count{};
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
                const auto a = std::hash<std::uint32_t>{}(key.bucket_slot);
                const auto b = std::hash<std::uint64_t>{}(ecs::entityBits(key.target));
                return a ^ (b + 0x9e3779b9U + (a << 6U) + (a >> 2U));
            }
        };
        struct EventRouteHead final
        {
            std::uint32_t first{NoPage};
            std::uint32_t last{NoPage};
        };
        using EventRouteIndex = entt::dense_map<EventRouteKey, EventRouteHead, EventRouteKeyHash>;

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
        [[nodiscard]] std::uint32_t appendPage(EventRouteHead& route) noexcept
        {
            // At most one nonempty page per ACTIVE wait. The source preflight
            // guarantees a free page without imposing a per-event capacity.
            if (free_page_ == NoPage) std::terminate();
            const auto index = free_page_;
            auto& page = pages_[index];
            free_page_ = page.next;
            page.previous = route.last;
            page.next = NoPage;
            page.count = 0U;
            if (route.last != NoPage) pages_[route.last].next = index;
            else route.first = index;
            route.last = index;
            return index;
        }
        void releasePage(std::uint32_t index) noexcept
        {
            auto& page = pages_[index];
            page.count = 0U;
            page.next = free_page_;
            free_page_ = index;
        }
        void unlinkRoute(Link& link) noexcept
        {
            auto& page = pages_[link.page_];
            const auto last = --page.count;
            if (link.position_ != last)
            {
                auto* moved = page.entries[last];
                page.entries[link.position_] = moved;
                moved->position_ = link.position_;
            }
            if (page.count != 0U) return;
            auto* route = findRoute(link.endpoint_, link.target_);
            if (route == nullptr) std::terminate();
            if (page.previous != NoPage) pages_[page.previous].next = page.next;
            else route->first = page.next;
            if (page.next != NoPage) pages_[page.next].previous = page.previous;
            else route->last = page.previous;
            releasePage(link.page_);
            if (route->first == NoPage) removeEmptyRoute(link.endpoint_, link.target_);
        }
        void claimRoute(std::uint32_t endpoint, ecs::Entity target) noexcept
        {
            ++claim_lookups_;
            auto* route = findRoute(endpoint, target);
            if (route == nullptr) return;
            auto current = route->first;
            // Each page contains a dense range. Task order inside one occurrence
            // is unspecified; cancellation swaps the last page entry into its gap.
            // Claim is still complete before any user callback can register again.
            while (current != NoPage)
            {
                auto& page = pages_[current];
                const auto next = page.next;
                for (std::uint32_t i{}; i < page.count; ++i)
                {
                    auto& link = *page.entries[i];
                    ++dispatch_visits_;
                    link.state_ = Link::EState::CLAIMED;
                    ++active_claimed_;
                    if (claimed_.size() == claimed_.capacity()) std::terminate();
                    claimed_.push_back(link.awaitable_);
                }
                releasePage(current);
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
                : owner_(std::exchange(other.owner_, nullptr)), endpoint_(other.endpoint_), target_(other.target_) {}
        private:
            friend class ScriptEventWaits;
            Admission(ScriptEventWaits& owner, std::uint32_t endpoint,
                ecs::Entity target) noexcept
                : owner_(&owner), endpoint_(endpoint), target_(target) {}
            ScriptEventWaits* owner_{};
            std::uint32_t endpoint_{};
            ecs::Entity target_{ecs::NullEntity};
        };
        class ClaimBatch final
        {
        public:
            ClaimBatch(const ClaimBatch&) = delete;
            ClaimBatch& operator=(const ClaimBatch&) = delete;
            ClaimBatch(ClaimBatch&& other) noexcept
                : owner_(std::exchange(other.owner_, nullptr)), begin_(other.begin_),
                  end_(other.end_), endpoint_(other.endpoint_) {}
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
                return {owner_->claimed_[begin_ + offset], endpoint_};
            }
        private:
            friend class ScriptEventWaits;
            ClaimBatch(ScriptEventWaits& owner, std::size_t begin, std::size_t end, std::uint32_t endpoint) noexcept
                : owner_(&owner), begin_(begin), end_(end), endpoint_(endpoint) {}
            ScriptEventWaits* owner_{};
            std::size_t begin_{};
            std::size_t end_{};
            std::uint32_t endpoint_{};
        };

        void prepare(std::size_t capacity, std::size_t endpoint_count);
        [[nodiscard]] lux::cxx::expected<Admission, EScriptEventWaitError> reserve(
            std::uint32_t endpoint, ecs::Entity target) noexcept
        {
            if (endpoint >= broadcast_routes_.size())
                return lux::cxx::unexpected(EScriptEventWaitError::UNDECLARED_SOURCE);
            const auto reserved = active_ - active_claimed_ + claimed_.size();
            if (reserved >= capacity_)
                return lux::cxx::unexpected(EScriptEventWaitError::WAITER_CAPACITY_EXCEEDED);
            return Admission{*this, endpoint, target};
        }
        [[nodiscard]] ScriptSourceId registerWait(
            Admission&& admission, Link& link, const ScriptWaitIdentity& wait) noexcept
        {
            if (std::exchange(admission.owner_, nullptr) != this || link.state_ != Link::EState::DETACHED)
                std::terminate();
            const auto endpoint = admission.endpoint_;
            const auto target = admission.target_;
            auto& route = target == ecs::NullEntity ? broadcast_routes_[endpoint] :
                routes_.try_emplace(EventRouteKey{endpoint, target}, EventRouteHead{}).first->second;
            link.awaitable_ = wait.id;
            link.endpoint_ = endpoint;
            link.target_ = target;
            link.state_ = Link::EState::ACTIVE;
            const auto page_index = route.last == NoPage || pages_[route.last].count == PageSize ?
                appendPage(route) : route.last;
            auto& page = pages_[page_index];
            link.page_ = page_index;
            link.position_ = static_cast<std::uint8_t>(page.count);
            page.entries[page.count++] = &link;
            high_water_ = (std::max)(high_water_, ++active_);
            return {wait.id.slot, wait.id.generation};
        }
        [[nodiscard]] ClaimBatch claim(std::uint32_t endpoint, ecs::Entity target) noexcept
        {
            const auto begin = claimed_.size();
            claimRoute(endpoint, target);
            return ClaimBatch{*this, begin, claimed_.size(), endpoint};
        }
        void cancel(Link& link) noexcept
        {
            if (link.state_ == Link::EState::DETACHED) return;
            if (link.state_ == Link::EState::ACTIVE) unlinkRoute(link);
            else --active_claimed_;
            link.state_ = Link::EState::DETACHED;
            --active_;
        }
        void cancelForRetirement(Link& link) noexcept
        {
            ++cleanup_visits_;
            cancel(link);
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
        std::vector<WaitPage> pages_;
        std::uint32_t free_page_{NoPage};
        EventRouteIndex routes_;
        std::vector<EventRouteHead> broadcast_routes_;
        std::vector<ScriptAwaitableId> claimed_;
        std::size_t capacity_{};
        std::size_t active_{};
        std::size_t active_claimed_{};
        std::size_t high_water_{};
        std::size_t dispatch_visits_{};
        std::size_t cleanup_visits_{};
        std::uint64_t claim_lookups_{};
    };
} // namespace lux::simulation::script::detail
