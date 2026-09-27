#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/simulation/HookPoint.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>

#include <entt/entity/sparse_set.hpp>

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::simulation::detail
{
    template <class Handler> class TDenseEntityHandlerStorage final
    {
        static_assert(std::is_nothrow_move_constructible_v<Handler>);
        static_assert(std::is_nothrow_move_assignable_v<Handler>);

        struct RegistrationTag;

        struct Registration final
        {
            ecs::Entity target{ecs::NullEntity};
            std::size_t dense_index{};
            bool connect_all{};
        };

        using RegistrationStorage = lux::cxx::SlotMap<Registration, RegistrationTag>;
        using RegistrationKey = typename RegistrationStorage::key_type;

        struct DenseHandler final
        {
            EndpointConnectionToken token;
            Handler value;
        };

        struct TargetBucket final
        {
            std::vector<DenseHandler> handlers;
        };

    public:
        [[nodiscard]] EEndpointMutationError prepare(std::size_t handler_capacity) noexcept
        {
            registrations_.clear();
            registrations_.reserve(handler_capacity);
            target_index_.clear();
            target_index_.reserve(handler_capacity);
            target_buckets_.clear();
            target_buckets_.reserve(handler_capacity);
            all_bucket_.handlers.clear();
            handler_capacity_ = handler_capacity;
            registration_lookups_ = 0U;
            return EEndpointMutationError::NONE;
        }

        [[nodiscard]] EndpointConnectResult connect(ecs::Entity target, Handler handler, bool connect_all) noexcept
        {
            if (registrations_.size() >= handler_capacity_)
            {
                return {{}, EEndpointMutationError::CAPACITY_EXCEEDED};
            }

            TargetBucket* bucket = std::addressof(all_bucket_);
            if (!connect_all)
            {
                if (target == ecs::NullEntity)
                {
                    return {{}, EEndpointMutationError::INVALID_TARGET};
                }
                if (!target_index_.contains(target))
                {
                    target_index_.push(target);
                    target_buckets_.push_back({});
                }
                bucket = std::addressof(targetBucket(target));
            }

            const auto inserted = registrations_.emplace(Registration{target, bucket->handlers.size(), connect_all});
            const auto token = toToken(inserted);
            bucket->handlers.push_back(DenseHandler{token, std::move(handler)});

            return {token, EEndpointMutationError::NONE};
        }

        [[nodiscard]] EEndpointMutationError disconnect(EndpointConnectionToken token) noexcept
        {
            if (!token.valid())
            {
                return EEndpointMutationError::INVALID_TOKEN;
            }
            const auto key = toKey(token);
            ++registration_lookups_;
            const auto* stored = registrations_.find(key);
            if (stored == nullptr)
            {
                return EEndpointMutationError::INVALID_TOKEN;
            }

            const Registration registration = *stored;
            auto& bucket = registration.connect_all ? all_bucket_ : targetBucket(registration.target);
            const auto last_index = bucket.handlers.size() - 1U;
            if (registration.dense_index != last_index)
            {
                bucket.handlers[registration.dense_index] = std::move(bucket.handlers.back());
                const auto moved_key = toKey(bucket.handlers[registration.dense_index].token);
                registrations_[moved_key].dense_index = registration.dense_index;
            }
            bucket.handlers.pop_back();
            registrations_.erase(key);
            if (!registration.connect_all && bucket.handlers.empty())
            {
                eraseTarget(registration.target);
            }
            return EEndpointMutationError::NONE;
        }

        template <class Invoke> void forEachAll(Invoke&& invoke) noexcept
        {
            for (auto& handler : all_bucket_.handlers)
            {
                invoke(handler.value);
            }
        }

        template <class Invoke> void forEachTarget(ecs::Entity target, Invoke&& invoke) noexcept
        {
            if (!target_index_.contains(target))
            {
                return;
            }
            for (auto& handler : targetBucket(target).handlers)
            {
                invoke(handler.value);
            }
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return registrations_.size();
        }

        [[nodiscard]] std::size_t targetBucketCount() const noexcept
        {
            return target_buckets_.size();
        }

        [[nodiscard]] std::size_t registrationLookupCount() const noexcept
        {
            return registration_lookups_;
        }

    private:
        [[nodiscard]] TargetBucket& targetBucket(ecs::Entity target) noexcept
        {
            return target_buckets_[target_index_.index(target)];
        }

        void eraseTarget(ecs::Entity target) noexcept
        {
            const auto index = target_index_.index(target);
            target_index_.erase(target);
            if (index + 1U != target_buckets_.size())
            {
                target_buckets_[index] = std::move(target_buckets_.back());
            }
            target_buckets_.pop_back();
        }

        [[nodiscard]] static constexpr EndpointConnectionToken toToken(RegistrationKey key) noexcept
        {
            return {key.index, key.gen};
        }

        [[nodiscard]] static constexpr RegistrationKey toKey(EndpointConnectionToken token) noexcept
        {
            return {token.slot, token.generation};
        }

        RegistrationStorage registrations_;
        entt::basic_sparse_set<ecs::Entity> target_index_;
        std::vector<TargetBucket> target_buckets_;
        TargetBucket all_bucket_;
        std::size_t handler_capacity_{};
        std::size_t registration_lookups_{};
    };
}
