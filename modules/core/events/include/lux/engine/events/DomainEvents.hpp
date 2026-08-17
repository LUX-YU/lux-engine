#pragma once
/**
 * @file DomainEvents.hpp
 * @brief Main-thread facts with host-owned delivery pumps.
 *
 * DomainEvents is deliberately not a cross-thread bus.  Async work must first
 * return through AsyncRuntime's MainThreadScheduler, commit authoritative state, and
 * only then publish a copyable fact here.  Each pump owns two vectors per event
 * type; swapping them at drain entry gives a precise "published while draining
 * is delivered next round" rule without a concurrent queue or a lock.
 */

#include <lux/engine/core/visibility.h>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/cxx/compile_time/type_info.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::events
{
    template <class Event>
    concept DomainEvent = std::movable<Event> && std::copyable<Event>;

    using EventTypeId = decltype(lux::cxx::type_hash<int>());

    template <class Event>
    inline constexpr EventTypeId kEventTypeId =
        lux::cxx::type_hash<Event>();

    enum class EOverflow : std::uint8_t
    {
        UNBOUNDED,
        DROP_NEWEST,
        DROP_OLDEST
    };

    struct ChannelConfig final
    {
        std::uint32_t capacity{0};
        EOverflow policy{EOverflow::UNBOUNDED};
    };

    struct ChannelDiag final
    {
        std::string_view event_type;
        std::string_view pump;
        std::uint64_t dropped{0};
        std::size_t depth{0};
    };

    class DomainEvents;
    class EventPump;

    namespace detail
    {
        class ChannelBase
        {
        public:
            virtual ~ChannelBase() = default;

            virtual void unsubscribe(
                void* per_pump,
                std::uint64_t subscription_id) noexcept = 0;
            virtual void collectDiagnostics(
                std::vector<ChannelDiag>& output) const = 0;
            [[nodiscard]] virtual std::size_t liveSubscriptions()
                const noexcept = 0;
            [[nodiscard]] virtual std::string_view typeName()
                const noexcept = 0;
        };

        template <DomainEvent Event>
        class Channel;
    }

    class Subscription final
    {
    public:
        Subscription() noexcept = default;
        ~Subscription() { reset(); }

        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        Subscription(Subscription&& other) noexcept
            : channel_(std::exchange(other.channel_, nullptr))
            , per_pump_(std::exchange(other.per_pump_, nullptr))
            , subscription_id_(
                  std::exchange(other.subscription_id_, 0u))
        {}

        Subscription& operator=(Subscription&& other) noexcept
        {
            if (this == &other)
                return *this;
            reset();
            channel_ = std::exchange(other.channel_, nullptr);
            per_pump_ = std::exchange(other.per_pump_, nullptr);
            subscription_id_ =
                std::exchange(other.subscription_id_, 0u);
            return *this;
        }

        void reset() noexcept
        {
            if (!channel_)
                return;
            channel_->unsubscribe(per_pump_, subscription_id_);
            channel_ = nullptr;
            per_pump_ = nullptr;
            subscription_id_ = 0u;
        }

        [[nodiscard]] bool active() const noexcept
        {
            return channel_ != nullptr;
        }

    private:
        template <DomainEvent Event>
        friend class detail::Channel;

        Subscription(
            detail::ChannelBase& channel,
            void* per_pump,
            std::uint64_t subscription_id) noexcept
            : channel_(&channel)
            , per_pump_(per_pump)
            , subscription_id_(subscription_id)
        {}

        detail::ChannelBase* channel_{nullptr};
        void* per_pump_{nullptr};
        std::uint64_t subscription_id_{0};
    };

    class SubscriptionGroup final
    {
    public:
        SubscriptionGroup() = default;
        ~SubscriptionGroup() { clear(); }

        SubscriptionGroup(const SubscriptionGroup&) = delete;
        SubscriptionGroup& operator=(const SubscriptionGroup&) = delete;
        SubscriptionGroup(SubscriptionGroup&&) noexcept = default;
        SubscriptionGroup& operator=(SubscriptionGroup&&) noexcept = default;

        void add(Subscription subscription)
        {
            subscriptions_.push_back(std::move(subscription));
        }

        void clear() noexcept
        {
            while (!subscriptions_.empty())
                subscriptions_.pop_back();
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return subscriptions_.size();
        }

    private:
        std::vector<Subscription> subscriptions_;
    };

    class EventPump final
    {
    public:
        EventPump(const EventPump&) = delete;
        EventPump& operator=(const EventPump&) = delete;
        EventPump(EventPump&&) = delete;
        EventPump& operator=(EventPump&&) = delete;

        LUX_CORE_PUBLIC void drain();
        LUX_CORE_PUBLIC void drainUntilEmpty(std::size_t max_rounds = 64u);

        [[nodiscard]] std::string_view name() const noexcept
        {
            return name_;
        }

    private:
        friend class DomainEvents;
        template <DomainEvent Event>
        friend class detail::Channel;

        struct Badge final
        {
        private:
            friend class DomainEvents;
            Badge() = default;
        };

    public:
        EventPump(Badge, DomainEvents& events, std::string name) noexcept
            : events_(&events), name_(std::move(name))
        {}

    private:
        using DrainFn = std::size_t (*)(detail::ChannelBase&, void*) noexcept;

        struct DrainEntry final
        {
            detail::ChannelBase* channel{nullptr};
            void* per_pump{nullptr};
            DrainFn drain{nullptr};
        };

        void addDrainEntry(
            detail::ChannelBase& channel,
            void* per_pump,
            DrainFn drain)
        {
            entries_.push_back(DrainEntry{&channel, per_pump, drain});
        }

        [[nodiscard]] std::size_t drainRound();

        DomainEvents* events_{nullptr};
        std::vector<DrainEntry> entries_;
        std::string name_;
        bool draining_{false};
    };

    namespace detail
    {
        template <DomainEvent Event>
        class Channel final : public ChannelBase
        {
        public:
            using HandlerFn =
                lux::cxx::move_only_function<void(const Event&)>;

            struct Handler final
            {
                std::uint64_t id{0};
                HandlerFn invoke;
                bool alive{true};
            };

            struct PerPump final
            {
                EventPump* pump{nullptr};
                std::vector<Handler> handlers;
                std::vector<Event> pending;
                std::vector<Event> current;
                ChannelConfig config{};
                std::uint64_t dropped{0};
                std::size_t live_handlers{0};
                bool in_drain{false};
            };

            explicit Channel(DomainEvents& owner) noexcept
                : owner_(&owner)
            {}

            void publish(Event event)
            {
                std::size_t destinations = 0;
                for (const auto& per_pump : per_pumps_)
                {
                    if (per_pump->live_handlers != 0u)
                        ++destinations;
                }
                if (destinations == 0u)
                    return;

                for (auto& per_pump : per_pumps_)
                {
                    if (per_pump->live_handlers == 0u)
                        continue;
                    if (per_pump->config.policy == EOverflow::DROP_NEWEST &&
                        per_pump->config.capacity != 0u &&
                        per_pump->pending.size() >=
                            per_pump->config.capacity)
                    {
                        ++per_pump->dropped;
                        --destinations;
                        continue;
                    }
                    if (destinations == 1u)
                        per_pump->pending.push_back(std::move(event));
                    else
                        per_pump->pending.push_back(event);
                    --destinations;
                }
            }

            [[nodiscard]] Subscription subscribe(
                EventPump& pump,
                HandlerFn handler)
            {
                ownerCheck();
                PerPump* per_pump = findPerPump(pump);
                if (!per_pump)
                {
                    auto storage = std::make_unique<PerPump>();
                    storage->pump = &pump;
                    storage->config = default_config_;
                    per_pump = storage.get();
                    per_pumps_.push_back(std::move(storage));
                    pump.addDrainEntry(
                        *this,
                        per_pump,
                        &Channel::drainThunk);
                }

                const auto id = next_subscription_id_++;
                per_pump->handlers.push_back(
                    Handler{id, std::move(handler), true});
                ++per_pump->live_handlers;
                ++live_subscriptions_;
                return Subscription{*this, per_pump, id};
            }

            void configure(ChannelConfig config)
            {
                ownerCheck();
                default_config_ = config;
                for (auto& per_pump : per_pumps_)
                    per_pump->config = config;
            }

            void unsubscribe(
                void* opaque,
                std::uint64_t subscription_id) noexcept override
            {
                ownerCheck();
                auto& per_pump = *static_cast<PerPump*>(opaque);
                for (auto& handler : per_pump.handlers)
                {
                    if (handler.id != subscription_id || !handler.alive)
                        continue;
                    handler.alive = false;
                    handler.invoke.reset();
                    --per_pump.live_handlers;
                    --live_subscriptions_;
                    if (!per_pump.in_drain)
                        compact(per_pump);
                    return;
                }
            }

            void collectDiagnostics(
                std::vector<ChannelDiag>& output) const override
            {
                for (const auto& per_pump : per_pumps_)
                {
                    output.push_back(ChannelDiag{
                        lux::cxx::type_name<Event>(),
                        per_pump->pump->name(),
                        per_pump->dropped,
                        per_pump->pending.size()});
                }
            }

            [[nodiscard]] std::size_t liveSubscriptions()
                const noexcept override
            {
                return live_subscriptions_;
            }

            [[nodiscard]] std::string_view typeName()
                const noexcept override
            {
                return lux::cxx::type_name<Event>();
            }

        private:
            [[nodiscard]] PerPump* findPerPump(EventPump& pump) noexcept
            {
                for (auto& per_pump : per_pumps_)
                {
                    if (per_pump->pump == &pump)
                        return per_pump.get();
                }
                return nullptr;
            }

            static std::size_t drainThunk(
                ChannelBase& base,
                void* opaque) noexcept
            {
                return static_cast<Channel&>(base).drainFor(
                    *static_cast<PerPump*>(opaque));
            }

            static std::size_t drainFor(PerPump& per_pump) noexcept
            {
                per_pump.current.clear();
                per_pump.current.swap(per_pump.pending);

                std::size_t first = 0u;
                if (per_pump.config.policy == EOverflow::DROP_OLDEST &&
                    per_pump.config.capacity != 0u &&
                    per_pump.current.size() > per_pump.config.capacity)
                {
                    first = per_pump.current.size() -
                        per_pump.config.capacity;
                    per_pump.dropped += first;
                }

                const std::size_t handler_limit =
                    per_pump.handlers.size();
                per_pump.in_drain = true;
                for (std::size_t event_index = first;
                     event_index < per_pump.current.size();
                     ++event_index)
                {
                    for (std::size_t handler_index = 0u;
                         handler_index < handler_limit;
                         ++handler_index)
                    {
                        auto& handler = per_pump.handlers[handler_index];
                        if (handler.alive && handler.invoke)
                            handler.invoke(per_pump.current[event_index]);
                    }
                }
                per_pump.in_drain = false;
                compact(per_pump);
                const auto processed = per_pump.current.size() - first;
                per_pump.current.clear();
                return processed;
            }

            static void compact(PerPump& per_pump) noexcept
            {
                std::erase_if(
                    per_pump.handlers,
                    [](const Handler& handler) noexcept
                    {
                        return !handler.alive;
                    });
            }

            void ownerCheck() const noexcept;

            DomainEvents* owner_{nullptr};
            std::vector<std::unique_ptr<PerPump>> per_pumps_;
            ChannelConfig default_config_{};
            std::uint64_t next_subscription_id_{1u};
            std::size_t live_subscriptions_{0u};
        };
    }

    class DomainEvents final
    {
    public:
        LUX_CORE_PUBLIC DomainEvents() noexcept;
        LUX_CORE_PUBLIC ~DomainEvents();

        DomainEvents(const DomainEvents&) = delete;
        DomainEvents& operator=(const DomainEvents&) = delete;
        DomainEvents(DomainEvents&&) = delete;
        DomainEvents& operator=(DomainEvents&&) = delete;

        template <DomainEvent Event>
        void publish(Event event)
        {
            ownerCheck();
            auto* channel = findChannel<Event>();
            if (channel)
                channel->publish(std::move(event));
        }

        [[nodiscard]] LUX_CORE_PUBLIC EventPump& createPump(std::string_view name);

        template <DomainEvent Event, class Handler>
        [[nodiscard]] Subscription subscribe(
            EventPump& pump,
            Handler&& handler)
        {
            ownerCheck();
            return channelOf<Event>().subscribe(
                pump,
                typename detail::Channel<Event>::HandlerFn{
                    std::forward<Handler>(handler)});
        }

        template <DomainEvent Event>
        void configure(ChannelConfig config)
        {
            ownerCheck();
            channelOf<Event>().configure(config);
        }

        [[nodiscard]] LUX_CORE_PUBLIC std::vector<ChannelDiag>
        diagnostics() const;

        [[nodiscard]] bool isOwnerThread() const noexcept
        {
            return std::this_thread::get_id() == owner_thread_;
        }

        void ownerCheck() const noexcept
        {
            if (!isOwnerThread())
                std::terminate();
        }

    private:
        template <DomainEvent Event>
        [[nodiscard]] detail::Channel<Event>* findChannel() noexcept
        {
            const auto found = channels_.find(kEventTypeId<Event>);
            if (found == channels_.end())
                return nullptr;
            if (found->second->typeName() != lux::cxx::type_name<Event>())
                std::terminate();
            return static_cast<detail::Channel<Event>*>(found->second.get());
        }

        template <DomainEvent Event>
        [[nodiscard]] detail::Channel<Event>& channelOf()
        {
            if (auto* existing = findChannel<Event>())
                return *existing;
            auto channel = std::make_unique<detail::Channel<Event>>(*this);
            auto* result = channel.get();
            channels_.emplace(kEventTypeId<Event>, std::move(channel));
            return *result;
        }

        std::thread::id owner_thread_;
        std::unordered_map<
            EventTypeId,
            std::unique_ptr<detail::ChannelBase>> channels_;
        std::deque<EventPump> pumps_;
    };

    namespace detail
    {
        template <DomainEvent Event>
        void Channel<Event>::ownerCheck() const noexcept
        {
            owner_->ownerCheck();
        }
    }
}
