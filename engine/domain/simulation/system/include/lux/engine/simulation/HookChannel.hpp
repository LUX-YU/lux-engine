#pragma once

#include <lux/engine/simulation/HookPoint.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace lux::simulation
{
    class SimulationBuilder;
    struct SimulationBroadcastRoute final {};
    template <class Target> struct EntityTargetedRoute final { using TargetType = Target; };

    struct HookChannelCapacity final
    {
        std::size_t producers{};
        std::size_t occurrences_per_producer{};
        std::size_t owner_occurrences{};
        std::size_t max_bytes{(std::numeric_limits<std::size_t>::max)()};
    };

    namespace detail
    {
        struct HookChannelProducerSlot final
        {
            std::size_t lane{};
            bool active{};
        };
        template <class Route> struct HookChannelTarget;
        template <> struct HookChannelTarget<SimulationBroadcastRoute> { using Type = std::monostate; };
        template <class Target> struct HookChannelTarget<EntityTargetedRoute<Target>> { using Type = Target; };
    }

    // Typed occurrence storage only. Subscribers and dispatch timing belong to the consuming Hook.
    template <class Route, class Payload>
    class HookChannel final
    {
        static_assert(std::is_nothrow_copy_constructible_v<Payload>);
        static_assert(std::is_nothrow_move_constructible_v<Payload>);
        static_assert(std::is_nothrow_destructible_v<Payload>);
        using Target = typename detail::HookChannelTarget<Route>::Type;

    public:
        // Non-scalar owners must explicitly declare their field-wise ownership copy.
        using OwnedCopy = Payload (*)(const Payload&) noexcept;
        struct Occurrence final
        {
            Target target;
            Payload payload;
        };

        class Writer final
        {
        public:
            Writer() noexcept = default;
            Writer(const Writer&) = delete;
            Writer& operator=(const Writer&) = delete;
            Writer(Writer&& other) noexcept
                : records_(std::exchange(other.records_, nullptr)), active_(other.active_), failed_(other.failed_),
                  capacity_(other.capacity_), copy_(other.copy_)
            {}
            Writer& operator=(Writer&& other) noexcept
            {
                if (this != &other)
                {
                    release();
                    records_ = std::exchange(other.records_, nullptr);
                    active_ = other.active_;
                    failed_ = other.failed_;
                    capacity_ = other.capacity_;
                    copy_ = other.copy_;
                }
                return *this;
            }
            ~Writer() noexcept { release(); }
            [[nodiscard]] bool record(Target target, Payload payload) noexcept
            {
                if (records_ == nullptr)
                    return false;
                if (records_->size() == capacity_)
                {
                    *failed_ = true;
                    return false;
                }
                records_->push_back({std::move(target), copy_(payload)});
                return true;
            }
            [[nodiscard]] bool record(Payload payload) noexcept
                requires std::is_same_v<Route, SimulationBroadcastRoute>
            {
                return record({}, std::move(payload));
            }

        private:
            Writer(std::vector<Occurrence>& records, bool& active, bool& failed,
                std::size_t capacity, OwnedCopy copy) noexcept
                : records_(&records), active_(&active), failed_(&failed), capacity_(capacity), copy_(copy)
            {
                active = true;
            }
            void release() noexcept
            {
                if (records_ != nullptr)
                    *active_ = false;
                records_ = nullptr;
            }
            std::vector<Occurrence>* records_{};
            bool* active_{};
            bool* failed_{};
            std::size_t capacity_{};
            OwnedCopy copy_{};
            friend class HookChannel;
        };

        class Producer final
        {
        public:
            Producer() noexcept = default;
            [[nodiscard]] Writer begin() const noexcept
            {
                if (channel_ == nullptr || slot_ == nullptr || !slot_->active)
                    return {};
                return channel_->beginPrepared(slot_->lane);
            }
        private:
            Producer(HookChannel* channel, detail::HookChannelProducerSlot* slot) noexcept
                : channel_(channel), slot_(slot)
            {}
            HookChannel* channel_{};
            detail::HookChannelProducerSlot* slot_{};
            friend class SimulationBuilder;
        };

        HookChannel() = default;
        HookChannel(const HookChannel&) = delete;
        HookChannel& operator=(const HookChannel&) = delete;
        HookChannel(HookChannel&&) = delete;
        HookChannel& operator=(HookChannel&&) = delete;

        [[nodiscard]] EEndpointMutationError prepare(HookChannelCapacity capacity, OwnedCopy copy = nullptr) noexcept
        {
            if (composed_)
                return EEndpointMutationError::DISPATCH_ACTIVE;
            if (sealed_)
                return EEndpointMutationError::DISPATCH_ACTIVE;
            if (writerActive())
                return EEndpointMutationError::WRITER_ACTIVE;
            if constexpr (std::is_arithmetic_v<Payload> || std::is_enum_v<Payload>)
            {
                if (copy == nullptr)
                    copy = [](const Payload& value) noexcept { return value; };
            }
            if (copy == nullptr)
                return EEndpointMutationError::PAYLOAD_NOT_OWNED;
            const auto max_records = capacity.max_bytes / sizeof(Occurrence);
            const bool is_invalid_capacity = capacity.producers == 0U || capacity.occurrences_per_producer == 0U ||
                capacity.producers > max_records / capacity.occurrences_per_producer;
            if (is_invalid_capacity)
                return EEndpointMutationError::CAPACITY_EXCEEDED;
            const auto remaining = max_records - capacity.producers * capacity.occurrences_per_producer;
            if (capacity.owner_occurrences > remaining / 2U)
                return EEndpointMutationError::CAPACITY_EXCEEDED;
            try
            {
                lanes_.clear();
                lanes_.resize(capacity.producers);
                for (auto& lane : lanes_)
                    lane.records.reserve(capacity.occurrences_per_producer);
                owner_.records.clear();
                deferred_.records.clear();
                owner_.records.reserve(capacity.owner_occurrences);
                deferred_.records.reserve(capacity.owner_occurrences);
                capacity_ = capacity;
                copy_ = copy;
                prepared_ = true;
                return EEndpointMutationError::NONE;
            }
            catch (const std::bad_alloc&)
            {
                prepared_ = false;
                return EEndpointMutationError::ALLOCATION_FAILURE;
            }
        }

        [[nodiscard]] Writer begin(std::size_t producer) noexcept
        {
            return composed_ ? Writer{} : beginPrepared(producer);
        }

        [[nodiscard]] Writer beginOwner() noexcept
        {
            return composed_ ? Writer{} : beginOwnerPrepared();
        }
        [[nodiscard]] Writer beginOwner(const HookInvocation& invocation) noexcept
        {
            const bool allowed = composed_ && invocation.owner_ == execution_owner_ &&
                invocation.system() == delivery_system_ && invocation.hook() == delivery_hook_;
            return allowed ? beginOwnerPrepared() : Writer{};
        }

    private:
        [[nodiscard]] Writer beginOwnerPrepared() noexcept
        {
            if (!prepared_ || capacity_.owner_occurrences == 0U)
                return {};
            auto& lane = sealed_ ? deferred_ : owner_;
            if (lane.active)
                return {};
            return Writer{lane.records, lane.active, lane.failed, capacity_.owner_occurrences, copy_};
        }

    public:
        [[nodiscard]] bool seal() noexcept
        {
            return !composed_ && sealPrepared();
        }

        [[nodiscard]] std::size_t laneCount() const noexcept
        {
            return lanes_.size() + (capacity_.owner_occurrences != 0U ? 1U : 0U);
        }
        [[nodiscard]] bool scriptConsumptionAllowed() const noexcept
        {
            return !composed_ || script_consumption_;
        }

        [[nodiscard]] std::span<const Occurrence> lane(std::size_t index) const noexcept
        {
            if (!sealed_ || index >= laneCount())
                return {};
            return index == lanes_.size() ? std::span<const Occurrence>{owner_.records} : lanes_[index].records;
        }

        [[nodiscard]] EEndpointMutationError mutationError() const noexcept
        {
            if (!prepared_)
                return EEndpointMutationError::NOT_PREPARED;
            if (sealed_)
                return EEndpointMutationError::DISPATCH_ACTIVE;
            return writerActive() ? EEndpointMutationError::WRITER_ACTIVE : EEndpointMutationError::NONE;
        }

        [[nodiscard]] bool failed() const noexcept
        {
            return owner_.failed || deferred_.failed ||
                std::ranges::any_of(lanes_, [](const auto& lane) noexcept { return lane.failed; });
        }

        void reset() noexcept
        {
            if (!composed_)
                resetPrepared();
        }

        void discard() noexcept
        {
            if (!composed_)
                discardPrepared();
        }

        [[nodiscard]] std::size_t pendingOccurrenceCount() const noexcept
        {
            auto count = owner_.records.size() + deferred_.records.size();
            for (const auto& lane : lanes_)
                count += lane.records.size();
            return count;
        }

    private:
        [[nodiscard]] Writer beginPrepared(std::size_t producer) noexcept
        {
            if (!prepared_ || sealed_ || producer >= lanes_.size() || lanes_[producer].active)
                return {};
            auto& lane = lanes_[producer];
            return Writer{lane.records, lane.active, lane.failed, capacity_.occurrences_per_producer, copy_};
        }
        [[nodiscard]] bool sealPrepared() noexcept
        {
            if (!prepared_ || sealed_ || writerActive() || failed())
                return false;
            sealed_ = true;
            return true;
        }
        void resetPrepared() noexcept
        {
            for (auto& lane : lanes_)
            {
                lane.records.clear();
                lane.failed = false;
            }
            owner_.records.clear();
            owner_.records.swap(deferred_.records);
            owner_.failed = deferred_.failed;
            deferred_.failed = false;
            sealed_ = false;
        }
        void discardPrepared() noexcept
        {
            deferred_.records.clear();
            deferred_.failed = false;
            resetPrepared();
        }
        struct Lane final
        {
            std::vector<Occurrence> records;
            bool active{};
            bool failed{};
        };

        [[nodiscard]] bool writerActive() const noexcept
        {
            return owner_.active || deferred_.active ||
                std::ranges::any_of(lanes_, [](const auto& lane) noexcept { return lane.active; });
        }

        std::vector<Lane> lanes_;
        Lane owner_;
        Lane deferred_;
        HookChannelCapacity capacity_;
        bool prepared_{};
        bool sealed_{};
        OwnedCopy copy_{};
        bool composed_{};
        bool script_consumption_{};
        const void* execution_owner_{};
        lux::system::SystemInstanceId delivery_system_;
        HookPointId delivery_hook_;
        friend class SimulationBuilder;
    };
}
