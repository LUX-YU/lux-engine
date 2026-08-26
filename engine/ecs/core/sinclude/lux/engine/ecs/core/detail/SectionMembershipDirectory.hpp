#pragma once

#include <lux/engine/ecs/World.hpp>

#include <entt/entity/entity.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <vector>

namespace lux::ecs::detail
{
    class SectionMembershipDirectory final
    {
      public:
        struct Stats final
        {
            std::uint64_t duplicate_comparisons{};
            std::size_t entry_capacity_bytes{};
            std::size_t node_capacity_bytes{};
            std::size_t active_tracked_entities{};
            std::size_t active_memberships{};
        };

        using NodeIndex = std::uint32_t;
        static constexpr NodeIndex InvalidNode =
            std::numeric_limits<NodeIndex>::max();

        [[nodiscard]] std::uint64_t allocateLease() noexcept
        {
            const std::uint64_t result = next_lease_++;
            require(result != 0U && next_lease_ != 0U);
            return result;
        }

        void reserve(
            std::span<const Entity> entities,
            std::size_t additional_memberships
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
            if (additional_memberships > nodes_.max_size() - nodes_.size())
                throw std::bad_alloc{};
            nodes_.reserve(nodes_.size() + additional_memberships);
        }

        void activate(
            std::uint64_t lease,
            std::span<const Entity> entities
        ) noexcept
        {
            require(lease != 0U);
            for (const Entity entity : entities)
            {
                Entry& entry = entries_[slot(entity)];
                require(entry.lease == 0U && entry.first == InvalidNode);
                entry.lease = lease;
                entry.generation = generation(entity);
                ++active_tracked_entities_;
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

        [[nodiscard]] NodeIndex prepareAdd(
            Entity entity,
            std::uint64_t storage
        )
        {
            if (!tracked(entity))
                return InvalidNode;
            for (NodeIndex node = entries_[slot(entity)].first;
                 node != InvalidNode;
                 node = nodes_[node].next)
            {
                require(nodes_[node].storage != storage);
                ++duplicate_comparisons_;
            }

            NodeIndex result{};
            if (free_ != InvalidNode)
            {
                result = free_;
                free_ = nodes_[result].next;
            }
            else
            {
                if (nodes_.size() >= InvalidNode)
                    throw std::bad_alloc{};
                result = static_cast<NodeIndex>(nodes_.size());
                nodes_.push_back(Node{});
            }
            nodes_[result] = Node{storage, InvalidNode};
            return result;
        }

        void commitAdd(Entity entity, NodeIndex node) noexcept
        {
            if (node == InvalidNode)
                return;
            require(tracked(entity) && node < nodes_.size());
            Entry& entry = entries_[slot(entity)];
            nodes_[node].next = entry.first;
            entry.first = node;
            ++active_memberships_;
        }

        void cancelAdd(NodeIndex node) noexcept
        {
            if (node == InvalidNode)
                return;
            require(node < nodes_.size());
            releaseNode(node);
        }

        void appendKnownUnique(
            Entity entity,
            std::uint64_t storage
        ) noexcept
        {
            require(tracked(entity));
            NodeIndex node{};
            if (free_ != InvalidNode)
            {
                node = free_;
                free_ = nodes_[node].next;
            }
            else
            {
                require(nodes_.size() < InvalidNode);
                require(nodes_.size() < nodes_.capacity());
                node = static_cast<NodeIndex>(nodes_.size());
                nodes_.push_back(Node{});
            }
            nodes_[node] = Node{storage, InvalidNode};
            commitAdd(entity, node);
        }

        void remove(Entity entity, std::uint64_t storage) noexcept
        {
            if (!tracked(entity))
                return;
            Entry& entry = entries_[slot(entity)];
            NodeIndex* link = &entry.first;
            while (*link != InvalidNode)
            {
                const NodeIndex node = *link;
                if (nodes_[node].storage == storage)
                {
                    *link = nodes_[node].next;
                    releaseNode(node);
                    require(active_memberships_ != 0U);
                    --active_memberships_;
                    return;
                }
                link = &nodes_[node].next;
            }
        }

        template <class Fn>
        void forEachStorage(Entity entity, Fn&& fn) const noexcept
        {
            require(tracked(entity));
            for (NodeIndex node = entries_[slot(entity)].first;
                 node != InvalidNode;
                 node = nodes_[node].next)
            {
                fn(nodes_[node].storage);
            }
        }

        void deactivate(Entity entity) noexcept
        {
            if (!tracked(entity))
                return;
            Entry& entry = entries_[slot(entity)];
            NodeIndex node = entry.first;
            while (node != InvalidNode)
            {
                const NodeIndex next = nodes_[node].next;
                releaseNode(node);
                require(active_memberships_ != 0U);
                --active_memberships_;
                node = next;
            }
            entry = {};
            require(active_tracked_entities_ != 0U);
            --active_tracked_entities_;
        }

        [[nodiscard]] Stats stats() const noexcept
        {
            return Stats{
                .duplicate_comparisons = duplicate_comparisons_,
                .entry_capacity_bytes = entries_.capacity() * sizeof(Entry),
                .node_capacity_bytes = nodes_.capacity() * sizeof(Node),
                .active_tracked_entities = active_tracked_entities_,
                .active_memberships = active_memberships_,
            };
        }

      private:
        struct Entry final
        {
            std::uint64_t lease{};
            std::uint32_t generation{};
            NodeIndex first{InvalidNode};
        };

        struct Node final
        {
            std::uint64_t storage{};
            NodeIndex next{InvalidNode};
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

        void releaseNode(NodeIndex node) noexcept
        {
            nodes_[node].storage = 0U;
            nodes_[node].next = free_;
            free_ = node;
        }

        std::vector<Entry> entries_;
        std::vector<Node> nodes_;
        NodeIndex free_{InvalidNode};
        std::uint64_t next_lease_{1U};
        std::uint64_t duplicate_comparisons_{};
        std::size_t active_tracked_entities_{};
        std::size_t active_memberships_{};
    };
} // namespace lux::ecs::detail
