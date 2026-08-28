#pragma once

#include <lux/engine/simulation/HookPoint.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>

#include <entt/entity/sparse_set.hpp>

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
        template <class Target, class Payload>
        class EventOccurrenceBuffer final
        {
            static_assert(std::is_nothrow_copy_constructible_v<Payload>);
            static_assert(std::is_nothrow_move_constructible_v<Payload>);
            static_assert(std::is_nothrow_destructible_v<Payload>);

        public:
            struct Occurrence final
            {
                Target target;
                Payload payload;
            };

            class Writer final
            {
            public:
                Writer() noexcept = default;
                Writer(const Writer &) = delete;
                Writer &operator=(const Writer &) = delete;

                Writer(Writer &&other) noexcept
                    : storage_(std::exchange(other.storage_, nullptr)), producer_(other.producer_)
                {
                }

                Writer &operator=(Writer &&other) noexcept
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
                    return storage_ != nullptr && storage_->record(producer_, std::move(target), std::move(payload));
                }

            private:
                Writer(EventOccurrenceBuffer &storage, std::size_t producer) noexcept
                    : storage_(&storage), producer_(producer)
                {
                    storage_->writer_active_[producer_] = 1U;
                }

                void release() noexcept
                {
                    if (storage_ == nullptr)
                        return;

                    storage_->writer_active_[producer_] = 0U;
                    storage_ = nullptr;
                }

                EventOccurrenceBuffer *storage_{};
                std::size_t producer_{};
                friend class EventOccurrenceBuffer;
            };

            EventOccurrenceBuffer() = default;
            EventOccurrenceBuffer(const EventOccurrenceBuffer &) = delete;
            EventOccurrenceBuffer &operator=(const EventOccurrenceBuffer &) = delete;
            EventOccurrenceBuffer(EventOccurrenceBuffer &&) = delete;
            EventOccurrenceBuffer &operator=(EventOccurrenceBuffer &&) = delete;

            [[nodiscard]] EEndpointMutationError prepare(
                std::size_t producer_count,
                std::size_t producer_capacity) noexcept
            {
                if (draining_)
                    return EEndpointMutationError::DISPATCH_ACTIVE;
                if (hasActiveWriter())
                    return EEndpointMutationError::WRITER_ACTIVE;

                try
                {
                    producers_.clear();
                    producers_.resize(producer_count);
                    for (auto &producer : producers_)
                        producer.reserve(producer_capacity);

                    writer_active_.assign(producer_count, 0U);
                    producer_capacity_ = producer_capacity;
                    prepared_ = true;
                    return EEndpointMutationError::NONE;
                }
                catch (const std::bad_alloc &)
                {
                    prepared_ = false;
                    return EEndpointMutationError::ALLOCATION_FAILURE;
                }
            }

            [[nodiscard]] Writer begin(std::size_t producer) noexcept
            {
                const bool is_invalid_producer = producer >= producers_.size();
                if (!prepared_ || draining_ || is_invalid_producer || writer_active_[producer] != 0U)
                    return {};
                return Writer(*this, producer);
            }

            [[nodiscard]] EEndpointMutationError mutationError() const noexcept
            {
                if (!prepared_)
                    return EEndpointMutationError::NOT_PREPARED;
                if (draining_)
                    return EEndpointMutationError::DISPATCH_ACTIVE;
                if (hasActiveWriter())
                    return EEndpointMutationError::WRITER_ACTIVE;
                return EEndpointMutationError::NONE;
            }

            template <class Invoke>
            [[nodiscard]] std::size_t drain(Invoke &&invoke) noexcept
            {
                if (mutationError() != EEndpointMutationError::NONE)
                    return 0U;

                draining_ = true;
                std::size_t calls{};
                for (auto &producer : producers_)
                {
                    for (auto &occurrence : producer)
                        calls += invoke(occurrence);
                    producer.clear();
                }
                draining_ = false;
                return calls;
            }

            [[nodiscard]] std::size_t pendingOccurrenceCount() const noexcept
            {
                std::size_t result{};
                for (const auto &producer : producers_)
                    result += producer.size();
                return result;
            }

        private:
            [[nodiscard]] bool record(std::size_t producer, Target target, Payload payload) noexcept
            {
                auto &values = producers_[producer];
                if (values.size() >= producer_capacity_)
                    return false;

                values.push_back({std::move(target), std::move(payload)});
                return true;
            }

            [[nodiscard]] bool hasActiveWriter() const noexcept
            {
                for (const auto active : writer_active_)
                {
                    if (active != 0U)
                        return true;
                }
                return false;
            }

            std::vector<std::vector<Occurrence>> producers_;
            std::vector<std::uint8_t> writer_active_;
            std::size_t producer_capacity_{};
            bool prepared_{};
            bool draining_{};
        };
    }

    template <class Payload>
    class EventPoint<SimulationBroadcastRoute, Payload> final
    {
    public:
        using Callback = void (*)(void *, const Payload &) noexcept;

    private:
        using Buffer = detail::EventOccurrenceBuffer<std::monostate, Payload>;
        using BufferWriter = typename Buffer::Writer;

        struct HandlerTag;

        struct Handler final
        {
            void *context{};
            Callback callback{};
        };

        using HandlerStorage = lux::cxx::SlotMap<Handler, HandlerTag>;
        using HandlerKey = typename HandlerStorage::key_type;

    public:
        class Writer final
        {
        public:
            Writer() noexcept = default;
            Writer(const Writer &) = delete;
            Writer &operator=(const Writer &) = delete;
            Writer(Writer &&) noexcept = default;
            Writer &operator=(Writer &&) noexcept = default;

            [[nodiscard]] bool record(Payload payload) noexcept
            {
                return writer_.record({}, std::move(payload));
            }

        private:
            explicit Writer(BufferWriter writer) noexcept : writer_(std::move(writer))
            {
            }

            BufferWriter writer_;
            friend class EventPoint;
        };

        EventPoint() = default;
        EventPoint(const EventPoint &) = delete;
        EventPoint &operator=(const EventPoint &) = delete;
        EventPoint(EventPoint &&) = delete;
        EventPoint &operator=(EventPoint &&) = delete;

        [[nodiscard]] EEndpointMutationError prepare(
            std::size_t producer_count,
            std::size_t producer_capacity,
            std::size_t handler_capacity) noexcept
        {
            const auto busy = buffer_.mutationError();
            if (prepared_ && busy != EEndpointMutationError::NONE)
                return busy;

            const auto prepared = buffer_.prepare(producer_count, producer_capacity);
            if (prepared != EEndpointMutationError::NONE)
                return prepared;

            try
            {
                handlers_.clear();
                handlers_.reserve(handler_capacity);
                handler_capacity_ = handler_capacity;
                prepared_ = true;
                return EEndpointMutationError::NONE;
            }
            catch (const std::bad_alloc &)
            {
                prepared_ = false;
                return EEndpointMutationError::ALLOCATION_FAILURE;
            }
        }

        [[nodiscard]] Writer begin(std::size_t producer) noexcept
        {
            return prepared_ ? Writer(buffer_.begin(producer)) : Writer{};
        }

        [[nodiscard]] EndpointConnectResult connect(void *context, Callback callback) noexcept
        {
            const auto error = topologyMutationError(callback);
            if (error != EEndpointMutationError::NONE)
                return {{}, error};
            if (handlers_.size() >= handler_capacity_)
                return {{}, EEndpointMutationError::CAPACITY_EXCEEDED};

            const auto inserted = handlers_.tryEmplace(Handler{context, callback});
            if (!inserted)
                return {{}, EEndpointMutationError::ALLOCATION_FAILURE};
            return {toToken(*inserted), EEndpointMutationError::NONE};
        }

        [[nodiscard]] EEndpointMutationError disconnect(EndpointConnectionToken token) noexcept
        {
            const auto error = topologyMutationError();
            if (error != EEndpointMutationError::NONE)
                return error;
            if (!token.valid() || !handlers_.erase(toKey(token)))
                return EEndpointMutationError::INVALID_TOKEN;
            return EEndpointMutationError::NONE;
        }

        [[nodiscard]] std::size_t drain() noexcept
        {
            return buffer_.drain(
                [&](const typename Buffer::Occurrence &occurrence) noexcept
                {
                    std::size_t calls{};
                    for (const auto &handler : handlers_.values())
                    {
                        handler.callback(handler.context, occurrence.payload);
                        ++calls;
                    }
                    return calls;
                }
            );
        }

        [[nodiscard]] std::size_t pendingOccurrenceCount() const noexcept
        {
            return buffer_.pendingOccurrenceCount();
        }

        [[nodiscard]] std::size_t handlerCount() const noexcept
        {
            return handlers_.size();
        }

    private:
        [[nodiscard]] EEndpointMutationError topologyMutationError(Callback callback) const noexcept
        {
            if (!prepared_)
                return EEndpointMutationError::NOT_PREPARED;

            const auto busy = buffer_.mutationError();
            if (busy != EEndpointMutationError::NONE)
                return busy;
            if (callback == nullptr)
                return EEndpointMutationError::INVALID_CALLBACK;
            return EEndpointMutationError::NONE;
        }

        [[nodiscard]] EEndpointMutationError topologyMutationError() const noexcept
        {
            if (!prepared_)
                return EEndpointMutationError::NOT_PREPARED;
            return buffer_.mutationError();
        }

        [[nodiscard]] static constexpr EndpointConnectionToken toToken(HandlerKey key) noexcept
        {
            return {key.index, key.gen};
        }

        [[nodiscard]] static constexpr HandlerKey toKey(EndpointConnectionToken token) noexcept
        {
            return {token.slot, token.generation};
        }

        Buffer buffer_;
        HandlerStorage handlers_;
        std::size_t handler_capacity_{};
        bool prepared_{};
    };

    template <class Payload>
    class EventPoint<EntityTargetedRoute<ecs::Entity>, Payload> final
    {
    public:
        using Callback = void (*)(void *, const ecs::Entity &, const Payload &) noexcept;

    private:
        using Buffer = detail::EventOccurrenceBuffer<ecs::Entity, Payload>;

        struct HandlerTag;

        struct Handler;
        using HandlerStorage = lux::cxx::SlotMap<Handler, HandlerTag>;
        using HandlerKey = typename HandlerStorage::key_type;

        struct Handler final
        {
            ecs::Entity target{ecs::NullEntity};
            void *context{};
            Callback callback{};
            bool match_all{};
            HandlerKey previous{HandlerKey::invalid()};
            HandlerKey next{HandlerKey::invalid()};
        };

        struct TargetBucket final
        {
            HandlerKey head{HandlerKey::invalid()};
            std::size_t size{};
        };

    public:
        using Writer = typename Buffer::Writer;

        EventPoint() = default;
        EventPoint(const EventPoint &) = delete;
        EventPoint &operator=(const EventPoint &) = delete;
        EventPoint(EventPoint &&) = delete;
        EventPoint &operator=(EventPoint &&) = delete;

        [[nodiscard]] EEndpointMutationError prepare(
            std::size_t producer_count,
            std::size_t producer_capacity,
            std::size_t handler_capacity) noexcept
        {
            const auto busy = buffer_.mutationError();
            if (prepared_ && busy != EEndpointMutationError::NONE)
                return busy;

            const auto prepared = buffer_.prepare(producer_count, producer_capacity);
            if (prepared != EEndpointMutationError::NONE)
                return prepared;

            try
            {
                handlers_.clear();
                handlers_.reserve(handler_capacity);
                target_index_.clear();
                target_index_.reserve(handler_capacity);
                target_buckets_.clear();
                target_buckets_.reserve(handler_capacity);
                all_bucket_ = {};
                handler_capacity_ = handler_capacity;
                prepared_ = true;
                return EEndpointMutationError::NONE;
            }
            catch (const std::bad_alloc &)
            {
                prepared_ = false;
                return EEndpointMutationError::ALLOCATION_FAILURE;
            }
        }

        [[nodiscard]] Writer begin(std::size_t producer) noexcept
        {
            return prepared_ ? buffer_.begin(producer) : Writer{};
        }

        [[nodiscard]] EndpointConnectResult connect(
            ecs::Entity target,
            void *context,
            Callback callback) noexcept
        {
            if (target == ecs::NullEntity)
                return {{}, EEndpointMutationError::INVALID_TARGET};
            return connectImpl(target, context, callback, false);
        }

        [[nodiscard]] EndpointConnectResult connectAll(void *context, Callback callback) noexcept
        {
            return connectImpl(ecs::NullEntity, context, callback, true);
        }

        [[nodiscard]] EEndpointMutationError disconnect(EndpointConnectionToken token) noexcept
        {
            const auto error = topologyMutationError();
            if (error != EEndpointMutationError::NONE)
                return error;
            if (!token.valid())
                return EEndpointMutationError::INVALID_TOKEN;

            const auto key = toKey(token);
            const auto *stored = handlers_.find(key);
            if (stored == nullptr)
                return EEndpointMutationError::INVALID_TOKEN;

            const Handler handler = *stored;
            auto &bucket = handler.match_all ? all_bucket_ : targetBucket(handler.target);
            if (handler.previous.isValid())
                handlers_[handler.previous].next = handler.next;
            else
                bucket.head = handler.next;
            if (handler.next.isValid())
                handlers_[handler.next].previous = handler.previous;

            --bucket.size;
            const bool remove_target = !handler.match_all && bucket.size == 0U;
            handlers_.erase(key);
            if (remove_target)
                eraseTarget(handler.target);
            return EEndpointMutationError::NONE;
        }

        [[nodiscard]] std::size_t drain() noexcept
        {
            return buffer_.drain(
                [&](const typename Buffer::Occurrence &occurrence) noexcept
                {
                    std::size_t calls = invokeBucket(all_bucket_, occurrence);
                    if (target_index_.contains(occurrence.target))
                        calls += invokeBucket(targetBucket(occurrence.target), occurrence);
                    return calls;
                }
            );
        }

        [[nodiscard]] std::size_t pendingOccurrenceCount() const noexcept
        {
            return buffer_.pendingOccurrenceCount();
        }

        [[nodiscard]] std::size_t handlerCount() const noexcept
        {
            return handlers_.size();
        }

        [[nodiscard]] std::size_t targetBucketCount() const noexcept
        {
            return target_buckets_.size();
        }

    private:
        [[nodiscard]] EndpointConnectResult connectImpl(
            ecs::Entity target,
            void *context,
            Callback callback,
            bool match_all) noexcept
        {
            const auto error = topologyMutationError(callback);
            if (error != EEndpointMutationError::NONE)
                return {{}, error};
            if (handlers_.size() >= handler_capacity_)
                return {{}, EEndpointMutationError::CAPACITY_EXCEEDED};

            bool inserted_target{};
            TargetBucket *bucket = &all_bucket_;
            if (!match_all)
            {
                if (!target_index_.contains(target))
                {
                    try
                    {
                        target_index_.push(target);
                        target_buckets_.push_back({});
                        inserted_target = true;
                    }
                    catch (const std::bad_alloc &)
                    {
                        return {{}, EEndpointMutationError::ALLOCATION_FAILURE};
                    }
                }
                bucket = &targetBucket(target);
            }

            const auto old_head = bucket->head;
            const auto inserted = handlers_.tryEmplace(
                Handler{
                    target,
                    context,
                    callback,
                    match_all,
                    HandlerKey::invalid(),
                    old_head
                }
            );
            if (!inserted)
            {
                if (inserted_target)
                    eraseTarget(target);
                return {{}, EEndpointMutationError::ALLOCATION_FAILURE};
            }

            if (old_head.isValid())
                handlers_[old_head].previous = *inserted;
            bucket->head = *inserted;
            ++bucket->size;
            return {toToken(*inserted), EEndpointMutationError::NONE};
        }

        [[nodiscard]] std::size_t invokeBucket(
            const TargetBucket &bucket,
            const typename Buffer::Occurrence &occurrence) noexcept
        {
            std::size_t calls{};
            auto key = bucket.head;
            while (key.isValid())
            {
                const auto *handler = handlers_.find(key);
                if (handler == nullptr)
                    break;

                const auto next = handler->next;
                handler->callback(handler->context, occurrence.target, occurrence.payload);
                ++calls;
                key = next;
            }
            return calls;
        }

        [[nodiscard]] TargetBucket &targetBucket(ecs::Entity target) noexcept
        {
            return target_buckets_[target_index_.index(target)];
        }

        [[nodiscard]] const TargetBucket &targetBucket(ecs::Entity target) const noexcept
        {
            return target_buckets_[target_index_.index(target)];
        }

        void eraseTarget(ecs::Entity target) noexcept
        {
            const auto index = target_index_.index(target);
            target_index_.erase(target);
            if (index + 1U != target_buckets_.size())
                target_buckets_[index] = std::move(target_buckets_.back());
            target_buckets_.pop_back();
        }

        [[nodiscard]] EEndpointMutationError topologyMutationError(Callback callback) const noexcept
        {
            if (!prepared_)
                return EEndpointMutationError::NOT_PREPARED;

            const auto busy = buffer_.mutationError();
            if (busy != EEndpointMutationError::NONE)
                return busy;
            if (callback == nullptr)
                return EEndpointMutationError::INVALID_CALLBACK;
            return EEndpointMutationError::NONE;
        }

        [[nodiscard]] EEndpointMutationError topologyMutationError() const noexcept
        {
            if (!prepared_)
                return EEndpointMutationError::NOT_PREPARED;
            return buffer_.mutationError();
        }

        [[nodiscard]] static constexpr EndpointConnectionToken toToken(HandlerKey key) noexcept
        {
            return {key.index, key.gen};
        }

        [[nodiscard]] static constexpr HandlerKey toKey(EndpointConnectionToken token) noexcept
        {
            return {token.slot, token.generation};
        }

        Buffer buffer_;
        HandlerStorage handlers_;
        entt::basic_sparse_set<ecs::Entity> target_index_;
        std::vector<TargetBucket> target_buckets_;
        TargetBucket all_bucket_;
        std::size_t handler_capacity_{};
        bool prepared_{};
    };
}
