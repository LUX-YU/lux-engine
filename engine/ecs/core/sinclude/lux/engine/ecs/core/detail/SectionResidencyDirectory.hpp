#pragma once

#include <lux/engine/ecs/World.hpp>

#include <entt/entity/entity.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace lux::ecs::detail
{
    using ResidencyMutationToken = std::uint64_t;

    struct SectionResidencyPort final
    {
        ResidencyMutationToken (*prepare_add)(
            void*, std::uint32_t, std::uint64_t
        );
        ResidencyMutationToken (*prepare_remove)(
            void*, std::uint32_t, std::uint64_t
        );
        void (*commit_mutation)(
            void*, std::uint32_t, ResidencyMutationToken
        ) noexcept;
        void (*cancel_mutation)(void*, ResidencyMutationToken) noexcept;
        void (*visit_actual)(
            const void*,
            std::uint32_t,
            void*,
            void (*)(void*, std::uint64_t) noexcept
        ) noexcept;
        void (*deactivate)(void*, std::uint32_t) noexcept;
    };

    class SectionResidencyDirectory final
    {
      public:
        [[nodiscard]] std::uint64_t allocateLease() noexcept
        {
            const std::uint64_t result = next_lease_++;
            require(result != 0U && next_lease_ != 0U);
            return result;
        }

        void reserve(
            std::span<const Entity> entities,
            std::size_t additional_owners
        )
        {
            std::size_t required_entries = entries_.size();
            for (const Entity entity : entities)
            {
                required_entries = std::max(
                    required_entries,
                    static_cast<std::size_t>(slot(entity)) + 1U
                );
            }
            entries_.resize(required_entries);
            owners_.reserve(owners_.size() + additional_owners);
        }

        void activate(
            std::uint64_t lease,
            std::shared_ptr<void> owner,
            void* context,
            const SectionResidencyPort& port,
            std::span<const Entity> entities
        ) noexcept
        {
            require(lease != 0U && owner && context != nullptr);
            require(owners_.size() < owners_.capacity());
            owners_.push_back(Owner{lease, std::move(owner)});
            for (std::size_t ordinal{}; ordinal < entities.size(); ++ordinal)
            {
                const Entity entity = entities[ordinal];
                Entry& entry = entries_[slot(entity)];
                require(entry.lease == 0U);
                entry = Entry{
                    context,
                    &port,
                    lease,
                    generation(entity),
                    static_cast<std::uint32_t>(ordinal)
                };
            }
        }

        [[nodiscard]] bool tracked(Entity entity) const noexcept
        {
            const std::size_t index = slot(entity);
            return index < entries_.size() &&
                entries_[index].lease != 0U &&
                entries_[index].generation == generation(entity);
        }

        [[nodiscard]] bool matches(
            Entity entity,
            std::uint64_t lease
        ) const noexcept
        {
            const std::size_t index = slot(entity);
            return index < entries_.size() &&
                entries_[index].lease == lease &&
                entries_[index].generation == generation(entity);
        }

        [[nodiscard]] ResidencyMutationToken prepareAdd(
            Entity entity,
            std::uint64_t storage
        )
        {
            if (!tracked(entity))
                return 0U;
            const Entry& entry = entries_[slot(entity)];
            return entry.port->prepare_add(
                entry.context,
                entry.ordinal,
                storage
            );
        }

        [[nodiscard]] ResidencyMutationToken prepareRemove(
            Entity entity,
            std::uint64_t storage
        )
        {
            if (!tracked(entity))
                return 0U;
            const Entry& entry = entries_[slot(entity)];
            return entry.port->prepare_remove(
                entry.context,
                entry.ordinal,
                storage
            );
        }

        void commitMutation(
            Entity entity,
            ResidencyMutationToken token
        ) noexcept
        {
            if (token == 0U)
                return;
            require(tracked(entity));
            const Entry& entry = entries_[slot(entity)];
            entry.port->commit_mutation(
                entry.context,
                entry.ordinal,
                token
            );
        }

        void cancelMutation(
            Entity entity,
            ResidencyMutationToken token
        ) noexcept
        {
            if (token == 0U)
                return;
            require(tracked(entity));
            const Entry& entry = entries_[slot(entity)];
            entry.port->cancel_mutation(entry.context, token);
        }

        template <class Fn>
        void forEachActualStorage(Entity entity, Fn&& fn) const noexcept
        {
            require(tracked(entity));
            const Entry& entry = entries_[slot(entity)];
            using Callback = std::remove_reference_t<Fn>;
            entry.port->visit_actual(
                entry.context,
                entry.ordinal,
                std::addressof(fn),
                [](void* context, std::uint64_t storage) noexcept
                {
                    (*static_cast<Callback*>(context))(storage);
                }
            );
        }

        void deactivate(Entity entity) noexcept
        {
            if (!tracked(entity))
                return;
            Entry& entry = entries_[slot(entity)];
            entry.port->deactivate(entry.context, entry.ordinal);
            entry = {};
        }

        void release(std::uint64_t lease) noexcept
        {
            const auto iterator = std::find_if(
                owners_.begin(),
                owners_.end(),
                [lease](const Owner& owner) noexcept
                {
                    return owner.lease == lease;
                }
            );
            require(iterator != owners_.end());
            *iterator = std::move(owners_.back());
            owners_.pop_back();
        }

        [[nodiscard]] std::size_t activeCount() const noexcept
        {
            return owners_.size();
        }

      private:
        struct Entry final
        {
            void* context{};
            const SectionResidencyPort* port{};
            std::uint64_t lease{};
            std::uint32_t generation{};
            std::uint32_t ordinal{};
        };

        struct Owner final
        {
            std::uint64_t lease{};
            std::shared_ptr<void> lifetime;
        };

        [[nodiscard]] static std::uint32_t slot(Entity entity) noexcept
        {
            return static_cast<std::uint32_t>(
                entt::entt_traits<Entity>::to_entity(entity)
            );
        }

        [[nodiscard]] static std::uint32_t generation(Entity entity) noexcept
        {
            return static_cast<std::uint32_t>(
                entt::entt_traits<Entity>::to_version(entity)
            );
        }

        std::vector<Entry> entries_;
        std::vector<Owner> owners_;
        std::uint64_t next_lease_{1U};
    };
} // namespace lux::ecs::detail
