#pragma once

#include <lux/engine/simulation/HookPoint.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace lux::simulation
{
    struct SimulationBroadcastRoute final
    {
    };

    template <class Target>
    struct EntityTargetedRoute final
    {
        using TargetType = Target;
    };

    template <class Route, class Payload>
    class EventPoint;

    namespace detail
    {
        template <class Target, class Payload, class Callback>
        class EventPointStorage final
        {
          public:
            struct Occurrence final
            {
                Target target;
                Payload payload;
            };

            struct Writer final
            {
                Writer() noexcept = default;
                Writer(const Writer&) = delete;
                Writer& operator=(const Writer&) = delete;

                Writer(Writer&& other) noexcept
                    : storage_(std::exchange(other.storage_, nullptr)),
                      producer_(other.producer_)
                {
                }

                Writer& operator=(Writer&& other) noexcept
                {
                    if (this == &other)
                        return *this;
                    release();
                    storage_ = std::exchange(other.storage_, nullptr);
                    producer_ = other.producer_;
                    return *this;
                }

                ~Writer()
                {
                    release();
                }

                [[nodiscard]] bool record(Target target, Payload payload) noexcept
                {
                    if (storage_ == nullptr)
                        return false;
                    auto& values = storage_->producers_[producer_];
                    if (values.size() >= storage_->producer_capacity_)
                        return false;
                    try
                    {
                        values.push_back({
                            std::move(target),
                            std::move(payload)});
                        return true;
                    }
                    catch (...)
                    {
                        return false;
                    }
                }

              private:
                Writer(EventPointStorage& storage, std::size_t producer) noexcept
                    : storage_(&storage), producer_(producer)
                {
                    ++storage_->active_writers_;
                }

                void release() noexcept
                {
                    if (storage_ == nullptr)
                        return;
                    --storage_->active_writers_;
                    storage_ = nullptr;
                }

                EventPointStorage* storage_{};
                std::size_t producer_{};
                friend class EventPointStorage;
            };

            EventPointStorage() = default;
            EventPointStorage(const EventPointStorage&) = delete;
            EventPointStorage& operator=(const EventPointStorage&) = delete;
            EventPointStorage(EventPointStorage&&) = delete;
            EventPointStorage& operator=(EventPointStorage&&) = delete;

            [[nodiscard]] EEndpointMutationError prepare(
                std::size_t producer_count,
                std::size_t producer_capacity,
                std::size_t handler_capacity,
                std::size_t mutation_capacity
            ) noexcept
            {
                if (active_writers_ != 0U)
                    return EEndpointMutationError::WRITER_ACTIVE;
                try
                {
                    producers_.clear();
                    producers_.resize(producer_count);
                    for (auto& producer : producers_)
                        producer.reserve(producer_capacity);
                    handlers_.clear();
                    handlers_.reserve(handler_capacity);
                    mutations_.clear();
                    mutations_.reserve(mutation_capacity);
                    producer_capacity_ = producer_capacity;
                    handler_capacity_ = handler_capacity;
                    mutation_capacity_ = mutation_capacity;
                    next_token_ = 1U;
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
                if (!prepared_ || draining_ || producer >= producers_.size())
                    return {};
                return Writer(*this, producer);
            }

            [[nodiscard]] EndpointConnectResult connect(
                Target target,
                void* context,
                Callback callback,
                bool match_all = false
            ) noexcept
            {
                if (!prepared_)
                    return {{}, EEndpointMutationError::NOT_PREPARED};
                if (callback == nullptr)
                    return {{}, EEndpointMutationError::INVALID_CALLBACK};
                const auto pending_connects =
                    static_cast<std::size_t>(std::count_if(
                        mutations_.begin(),
                        mutations_.end(),
                        [](const Mutation& value) noexcept
                        {
                            return value.connect;
                        }
                    ));
                if (handlers_.size() + pending_connects >= handler_capacity_ ||
                    mutations_.size() >= mutation_capacity_)
                {
                    return {{}, EEndpointMutationError::CAPACITY_EXCEEDED};
                }
                auto token = EndpointConnectionToken{next_token_++};
                if (!token.valid())
                    token = EndpointConnectionToken{next_token_++};
                mutations_.push_back({
                    true,
                    token,
                    std::move(target),
                    context,
                    callback,
                    match_all});
                return {token, EEndpointMutationError::NONE};
            }

            [[nodiscard]] EEndpointMutationError disconnect(
                EndpointConnectionToken token
            ) noexcept
            {
                if (!prepared_)
                    return EEndpointMutationError::NOT_PREPARED;
                if (!token.valid())
                    return EEndpointMutationError::INVALID_TOKEN;
                if (mutations_.size() >= mutation_capacity_)
                    return EEndpointMutationError::CAPACITY_EXCEEDED;
                mutations_.push_back({
                    false,
                    token,
                    {},
                    nullptr,
                    nullptr,
                    false});
                return EEndpointMutationError::NONE;
            }

            [[nodiscard]] EEndpointMutationError flushMutations() noexcept
            {
                if (!prepared_)
                    return EEndpointMutationError::NOT_PREPARED;
                if (active_writers_ != 0U)
                    return EEndpointMutationError::WRITER_ACTIVE;
                if (draining_)
                    return EEndpointMutationError::DISPATCH_ACTIVE;
                for (auto& mutation : mutations_)
                {
                    if (mutation.connect)
                    {
                        handlers_.push_back({
                            mutation.token,
                            std::move(mutation.target),
                            mutation.context,
                            mutation.callback,
                            mutation.match_all,
                            true});
                        continue;
                    }
                    const auto found = std::find_if(
                        handlers_.begin(),
                        handlers_.end(),
                        [&](const Handler& handler) noexcept
                        {
                            return handler.token == mutation.token;
                        }
                    );
                    if (found != handlers_.end())
                        found->active = false;
                }
                mutations_.clear();
                handlers_.erase(
                    std::remove_if(
                        handlers_.begin(),
                        handlers_.end(),
                        [](const Handler& handler) noexcept
                        {
                            return !handler.active;
                        }
                    ),
                    handlers_.end()
                );
                return EEndpointMutationError::NONE;
            }

            template <class Invoke>
            [[nodiscard]] std::size_t drain(Invoke&& invoke) noexcept
            {
                if (!prepared_ || active_writers_ != 0U || draining_)
                    return 0U;
                draining_ = true;
                std::size_t calls{};
                for (auto& producer : producers_)
                {
                    for (auto& occurrence : producer)
                    {
                        for (const auto& handler : handlers_)
                        {
                            if (!handler.active ||
                                (!handler.match_all &&
                                !invoke.matches(handler.target, occurrence.target))
                                )
                            {
                                continue;
                            }
                            invoke.call(
                                handler.callback,
                                handler.context,
                                occurrence.target,
                                occurrence.payload
                            );
                            ++calls;
                        }
                    }
                    producer.clear();
                }
                draining_ = false;
                return calls;
            }

            [[nodiscard]] std::size_t pendingOccurrenceCount() const noexcept
            {
                std::size_t result{};
                for (const auto& producer : producers_)
                    result += producer.size();
                return result;
            }

          private:
            struct Handler final
            {
                EndpointConnectionToken token;
                Target target;
                void* context{};
                Callback callback{};
                bool match_all{};
                bool active{};
            };

            struct Mutation final
            {
                bool connect{};
                EndpointConnectionToken token;
                Target target;
                void* context{};
                Callback callback{};
                bool match_all{};
            };

            std::vector<std::vector<Occurrence>> producers_;
            std::vector<Handler> handlers_;
            std::vector<Mutation> mutations_;
            std::size_t producer_capacity_{};
            std::size_t handler_capacity_{};
            std::size_t mutation_capacity_{};
            std::size_t active_writers_{};
            std::uint64_t next_token_{1U};
            bool prepared_{};
            bool draining_{};
        };
    }

    template <class Payload>
    class EventPoint<SimulationBroadcastRoute, Payload> final
    {
      public:
        using Callback = void (*)(void*, const Payload&) noexcept;

      private:
        using Storage = detail::EventPointStorage<
            std::monostate,
            Payload,
            Callback>;

        struct Invoke final
        {
            [[nodiscard]] bool matches(
                std::monostate,
                std::monostate
            ) const noexcept
            {
                return true;
            }

            void call(
                Callback callback,
                void* context,
                std::monostate,
                const Payload& payload
            ) const noexcept
            {
                callback(context, payload);
            }
        };

      public:
        using WriterStorage = typename Storage::Writer;

        class Writer final
        {
          public:
            Writer() noexcept = default;
            Writer(const Writer&) = delete;
            Writer& operator=(const Writer&) = delete;
            Writer(Writer&&) noexcept = default;
            Writer& operator=(Writer&&) noexcept = default;

            [[nodiscard]] bool record(Payload payload) noexcept
            {
                return writer_.record({}, std::move(payload));
            }

          private:
            explicit Writer(WriterStorage writer) noexcept
                : writer_(std::move(writer))
            {
            }
            WriterStorage writer_;
            friend class EventPoint;
        };

        EventPoint() = default;
        EventPoint(const EventPoint&) = delete;
        EventPoint& operator=(const EventPoint&) = delete;
        EventPoint(EventPoint&&) = delete;
        EventPoint& operator=(EventPoint&&) = delete;

        [[nodiscard]] EEndpointMutationError prepare(
            std::size_t producer_count,
            std::size_t producer_capacity,
            std::size_t handler_capacity,
            std::size_t mutation_capacity
        ) noexcept
        {
            return storage_.prepare(
                producer_count,
                producer_capacity,
                handler_capacity,
                mutation_capacity
            );
        }

        [[nodiscard]] Writer begin(std::size_t producer) noexcept
        {
            return Writer(storage_.begin(producer));
        }

        [[nodiscard]] EndpointConnectResult connect(
            void* context,
            Callback callback
        ) noexcept
        {
            return storage_.connect({}, context, callback);
        }

        [[nodiscard]] EEndpointMutationError disconnect(
            EndpointConnectionToken token
        ) noexcept
        {
            return storage_.disconnect(token);
        }

        [[nodiscard]] EEndpointMutationError flushMutations() noexcept
        {
            return storage_.flushMutations();
        }

        [[nodiscard]] std::size_t drain() noexcept
        {
            return storage_.drain(Invoke{});
        }

        [[nodiscard]] std::size_t pendingOccurrenceCount() const noexcept
        {
            return storage_.pendingOccurrenceCount();
        }

      private:
        Storage storage_;
    };

    template <class Target, class Payload>
    class EventPoint<EntityTargetedRoute<Target>, Payload> final
    {
      public:
        using Callback = void (*)(
            void*,
            const Target&,
            const Payload&
        ) noexcept;

      private:
        using Storage = detail::EventPointStorage<Target, Payload, Callback>;

        struct Invoke final
        {
            [[nodiscard]] bool matches(
                const Target& subscribed,
                const Target& occurrence
            ) const noexcept
            {
                return subscribed == occurrence;
            }

            void call(
                Callback callback,
                void* context,
                const Target& target,
                const Payload& payload
            ) const noexcept
            {
                callback(context, target, payload);
            }
        };

      public:
        using Writer = typename Storage::Writer;

        EventPoint() = default;
        EventPoint(const EventPoint&) = delete;
        EventPoint& operator=(const EventPoint&) = delete;
        EventPoint(EventPoint&&) = delete;
        EventPoint& operator=(EventPoint&&) = delete;

        [[nodiscard]] EEndpointMutationError prepare(
            std::size_t producer_count,
            std::size_t producer_capacity,
            std::size_t handler_capacity,
            std::size_t mutation_capacity
        ) noexcept
        {
            return storage_.prepare(
                producer_count,
                producer_capacity,
                handler_capacity,
                mutation_capacity
            );
        }

        [[nodiscard]] Writer begin(std::size_t producer) noexcept
        {
            return storage_.begin(producer);
        }

        [[nodiscard]] EndpointConnectResult connect(
            Target target,
            void* context,
            Callback callback
        ) noexcept
        {
            return storage_.connect(std::move(target), context, callback);
        }

        [[nodiscard]] EndpointConnectResult connectAll(
            void* context,
            Callback callback
        ) noexcept
        {
            return storage_.connect({}, context, callback, true);
        }

        [[nodiscard]] EEndpointMutationError disconnect(
            EndpointConnectionToken token
        ) noexcept
        {
            return storage_.disconnect(token);
        }

        [[nodiscard]] EEndpointMutationError flushMutations() noexcept
        {
            return storage_.flushMutations();
        }

        [[nodiscard]] std::size_t drain() noexcept
        {
            return storage_.drain(Invoke{});
        }

        [[nodiscard]] std::size_t pendingOccurrenceCount() const noexcept
        {
            return storage_.pendingOccurrenceCount();
        }

      private:
        Storage storage_;
    };
}
