#pragma once

#include <lux/engine/ecs/RegistryStorageCapacity.hpp>
#include <lux/engine/ecs/components/ParentComponent.hpp>
#include <lux/engine/ecs/Registry.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace lux::ecs
{
    namespace detail
    {
        /// Internal lifetime marker for every entity that participates in a
        /// hierarchy as either a child or a parent. A parent does not itself
        /// need a ParentComponent, so this marker is what makes destroying a
        /// root invalidate the shared topology index.
        struct HierarchyMembershipState final {};

        inline void ensureHierarchyMembership(
            lux::ecs::RegistryBase& registry,
            lux::ecs::Entity entity)
        {
            if (entity != lux::ecs::kNullEntity && registry.valid(entity) &&
                !registry.all_of<HierarchyMembershipState>(entity))
            {
                registry.emplace<HierarchyMembershipState>(entity);
            }
        }
    } // namespace detail

    struct HierarchySubtreeRange final
    {
        std::uint32_t first{0u};
        std::uint32_t count{0u};
    };

    /// The registry's single hierarchy index. Parent writes and participating
    /// entity destruction only invalidate topology; the first owner-thread
    /// reader after a command barrier rebuilds children, deterministic preorder
    /// and subtree ranges together.
    class HierarchyIndex final
    {
    public:
        explicit HierarchyIndex(
            lux::ecs::RegistryBase& registry) noexcept
            : registry_(&registry)
        {
        }

        HierarchyIndex(const HierarchyIndex&) = delete;
        HierarchyIndex& operator=(const HierarchyIndex&) = delete;

        void markTopologyDirty() noexcept { topology_dirty_ = true; }

        /// Preflights the contiguous buffers used by a topology rebuild and
        /// the EnTT packed/payload capacity for hierarchy membership. A
        /// rebuild after publication then only clears and fills these owned
        /// buffers. This does not reserve every EnTT sparse entity-index page;
        /// a no-system-heap barrier additionally requires a registry allocator
        /// reservation contract.
        [[nodiscard]] bool reserveForAdditionalEdges(
            std::size_t additional) noexcept
        {
            const auto& parents =
                registry_->storage<ParentComponent>();
            const auto live = parents.size();
            if (additional > std::numeric_limits<std::size_t>::max() - live)
                return false;
            // Command preflight reserves ParentComponent cumulatively across
            // the complete barrier before asking the hierarchy index to grow.
            // Following that packed high-water mark covers several ARMED
            // commands while remaining stable across same-capacity churn.
            const auto edges = std::max(
                live + additional,
                parents.capacity());
            if (edges > std::numeric_limits<std::size_t>::max() / 2u)
                return false;
            const auto nodes = edges * 2u;
            auto& membership =
                registry_->storage<detail::HierarchyMembershipState>();
            if (additional >
                (std::numeric_limits<std::size_t>::max() -
                    membership.size()) / 2u)
            {
                return false;
            }
            const auto membership_capacity = std::max(
                nodes,
                membership.size() + additional * 2u);
            if (!reserveStorageCapacity(
                    membership, membership_capacity))
            {
                return false;
            }
            parent_entries_.reserve(edges);
            child_edges_.reserve(edges);
            children_.reserve(edges);
            child_ranges_.reserve(edges);
            nodes_.reserve(nodes);
            roots_.reserve(nodes);
            preorder_.reserve(nodes);
            subtree_ranges_.reserve(nodes);
            visit_state_.reserve(nodes);
            dfs_stack_.reserve(nodes);
            return true;
        }

        /// Returns true exactly when this call rebuilt topology.
        [[nodiscard]] bool refresh()
        {
            if (!topology_dirty_)
                return false;
            rebuild();
            return true;
        }

        [[nodiscard]] bool topologyDirty() const noexcept
        {
            return topology_dirty_;
        }

        [[nodiscard]] std::uint64_t topologyRevision() const noexcept
        {
            return topology_revision_;
        }

        [[nodiscard]] std::uint64_t rebuildCount() const noexcept
        {
            return rebuild_count_;
        }

        [[nodiscard]] std::size_t unresolvedCount()
        {
            (void)refresh();
            return unresolved_count_;
        }

        [[nodiscard]] std::span<const lux::ecs::Entity> preorder()
        {
            (void)refresh();
            return preorder_;
        }

        [[nodiscard]] std::optional<HierarchySubtreeRange> subtreeRange(
            lux::ecs::Entity entity)
        {
            (void)refresh();
            const auto found = findSubtreeRange(entity);
            if (found != subtree_ranges_.end() && found->entity == entity)
                return found->range;
            return std::nullopt;
        }

        [[nodiscard]] std::span<const lux::ecs::Entity> childrenOf(
            lux::ecs::Entity entity)
        {
            (void)refresh();
            return childrenOfReady(entity);
        }

        [[nodiscard]] bool hasChildren(lux::ecs::Entity entity)
        {
            return !childrenOf(entity).empty();
        }

        [[nodiscard]] lux::ecs::Entity parentOf(
            lux::ecs::Entity entity)
        {
            (void)refresh();
            const auto found = findParent(entity);
            if (found != parent_entries_.end() && found->child == entity)
                return found->parent;
            return lux::ecs::kNullEntity;
        }

        template <class Visit>
        void forEachInSubtree(lux::ecs::Entity root, Visit&& visit)
        {
            static_cast<void>(refresh());
            if (const auto range = subtreeRangeReady(root))
            {
                const auto first = range->first;
                const auto last = first + range->count;
                for (auto index = first; index < last; ++index)
                    visit(preorder_[index]);
                return;
            }
            visit(root);
        }

#ifndef NDEBUG
        void validate()
        {
            static_cast<void>(refresh());
            std::size_t live_edges = 0u;
            for (const auto child : registry_->view<const ParentComponent>())
            {
                const auto parent = registry_->get<const ParentComponent>(child).parent();
                const auto found = findParent(child);
                assert(found != parent_entries_.end() &&
                    found->child == child && found->parent == parent &&
                    "HierarchyIndex does not match ParentComponent storage");
                ++live_edges;
            }
            assert(live_edges == parent_entries_.size());
        }
#endif

    private:
        struct ParentEntry final
        {
            lux::ecs::Entity child{lux::ecs::kNullEntity};
            lux::ecs::Entity parent{lux::ecs::kNullEntity};
        };

        struct ChildEdge final
        {
            lux::ecs::Entity parent{lux::ecs::kNullEntity};
            lux::ecs::Entity child{lux::ecs::kNullEntity};
        };

        struct ChildRange final
        {
            lux::ecs::Entity parent{lux::ecs::kNullEntity};
            std::uint32_t first{0u};
            std::uint32_t count{0u};
        };

        struct SubtreeEntry final
        {
            lux::ecs::Entity entity{lux::ecs::kNullEntity};
            HierarchySubtreeRange range{};
        };

        struct DfsFrame final
        {
            lux::ecs::Entity entity{lux::ecs::kNullEntity};
            std::size_t next_child{0u};
            std::uint32_t first{0u};
        };

        void rebuild()
        {
            parent_entries_.clear();
            child_edges_.clear();
            children_.clear();
            child_ranges_.clear();
            subtree_ranges_.clear();
            nodes_.clear();
            roots_.clear();
            preorder_.clear();
            dfs_stack_.clear();
            visit_state_.clear();

            for (const auto child : registry_->view<const ParentComponent>())
            {
                const auto parent = registry_->get<const ParentComponent>(child).parent();
                parent_entries_.push_back({child, parent});
                nodes_.push_back(child);
                if (registry_->valid(parent))
                {
                    nodes_.push_back(parent);
                    child_edges_.push_back({parent, child});
                }
            }

            std::sort(
                parent_entries_.begin(),
                parent_entries_.end(),
                [](const ParentEntry& left, const ParentEntry& right)
                {
                    return entityLess(left.child, right.child);
                });
            std::sort(
                child_edges_.begin(),
                child_edges_.end(),
                [](const ChildEdge& left, const ChildEdge& right)
                {
                    return left.parent == right.parent
                        ? entityLess(left.child, right.child)
                        : entityLess(left.parent, right.parent);
                });
            std::sort(nodes_.begin(), nodes_.end(), entityLess);
            nodes_.erase(std::unique(nodes_.begin(), nodes_.end()), nodes_.end());

            for (std::size_t first = 0u; first < child_edges_.size();)
            {
                const auto parent = child_edges_[first].parent;
                const auto child_first = static_cast<std::uint32_t>(
                    children_.size());
                auto cursor = first;
                do
                {
                    children_.push_back(child_edges_[cursor].child);
                    ++cursor;
                }
                while (cursor < child_edges_.size() &&
                    child_edges_[cursor].parent == parent);
                child_ranges_.push_back({
                    parent,
                    child_first,
                    static_cast<std::uint32_t>(cursor - first)});
                first = cursor;
            }

            for (const auto entity : nodes_)
            {
                const auto found = findParent(entity);
                if (found == parent_entries_.end() ||
                    found->child != entity ||
                    !registry_->valid(found->parent) ||
                    !std::binary_search(
                        nodes_.begin(),
                        nodes_.end(),
                        found->parent,
                        entityLess))
                {
                    roots_.push_back(entity);
                }
            }

            visit_state_.resize(nodes_.size(), 0u);
            for (const auto root : roots_)
                appendSubtree(root);

            std::sort(
                subtree_ranges_.begin(),
                subtree_ranges_.end(),
                [](const SubtreeEntry& left, const SubtreeEntry& right)
                {
                    return entityLess(left.entity, right.entity);
                });

            unresolved_count_ = nodes_.size() - preorder_.size();
            topology_dirty_ = false;
            ++topology_revision_;
            ++rebuild_count_;
        }

        void appendSubtree(lux::ecs::Entity root)
        {
            auto* root_state = visitState(root);
            if (!root_state || *root_state != 0u)
                return;
            *root_state = 1u;
            const auto first = static_cast<std::uint32_t>(preorder_.size());
            preorder_.push_back(root);
            dfs_stack_.push_back({root, 0u, first});

            while (!dfs_stack_.empty())
            {
                auto& frame = dfs_stack_.back();
                const auto children = childrenOfReady(frame.entity);
                if (frame.next_child < children.size())
                {
                    const auto child = children[frame.next_child++];
                    auto* state = visitState(child);
                    if (!state || *state != 0u)
                        continue;
                    *state = 1u;
                    const auto child_first =
                        static_cast<std::uint32_t>(preorder_.size());
                    preorder_.push_back(child);
                    dfs_stack_.push_back({child, 0u, child_first});
                    continue;
                }

                subtree_ranges_.push_back({
                    frame.entity,
                    {frame.first,
                     static_cast<std::uint32_t>(preorder_.size()) -
                         frame.first}});
                if (auto* state = visitState(frame.entity))
                    *state = 2u;
                dfs_stack_.pop_back();
            }
        }

        [[nodiscard]] std::vector<ParentEntry>::iterator findParent(
            lux::ecs::Entity entity)
        {
            return std::lower_bound(
                parent_entries_.begin(),
                parent_entries_.end(),
                entity,
                [](const ParentEntry& entry, lux::ecs::Entity value)
                {
                    return entityLess(entry.child, value);
                });
        }

        [[nodiscard]] std::vector<ParentEntry>::const_iterator findParent(
            lux::ecs::Entity entity) const
        {
            return std::lower_bound(
                parent_entries_.begin(),
                parent_entries_.end(),
                entity,
                [](const ParentEntry& entry, lux::ecs::Entity value)
                {
                    return entityLess(entry.child, value);
                });
        }

        [[nodiscard]] std::vector<ChildRange>::const_iterator findChildRange(
            lux::ecs::Entity entity) const
        {
            return std::lower_bound(
                child_ranges_.begin(),
                child_ranges_.end(),
                entity,
                [](const ChildRange& entry, lux::ecs::Entity value)
                {
                    return entityLess(entry.parent, value);
                });
        }

        [[nodiscard]] std::span<const lux::ecs::Entity> childrenOfReady(
            lux::ecs::Entity entity) const noexcept
        {
            const auto found = findChildRange(entity);
            if (found == child_ranges_.end() || found->parent != entity)
                return {};
            return std::span<const lux::ecs::Entity>{
                children_.data() + found->first,
                found->count};
        }

        [[nodiscard]] std::vector<SubtreeEntry>::const_iterator
        findSubtreeRange(lux::ecs::Entity entity) const
        {
            return std::lower_bound(
                subtree_ranges_.begin(),
                subtree_ranges_.end(),
                entity,
                [](const SubtreeEntry& entry, lux::ecs::Entity value)
                {
                    return entityLess(entry.entity, value);
                });
        }

        [[nodiscard]] std::optional<HierarchySubtreeRange> subtreeRangeReady(
            lux::ecs::Entity entity) const noexcept
        {
            const auto found = findSubtreeRange(entity);
            if (found == subtree_ranges_.end() || found->entity != entity)
                return std::nullopt;
            return found->range;
        }

        [[nodiscard]] std::uint8_t* visitState(
            lux::ecs::Entity entity) noexcept
        {
            const auto found = std::lower_bound(
                nodes_.begin(), nodes_.end(), entity, entityLess);
            if (found == nodes_.end() || *found != entity)
                return nullptr;
            return &visit_state_[static_cast<std::size_t>(
                found - nodes_.begin())];
        }

        [[nodiscard]] static bool entityLess(
            lux::ecs::Entity left,
            lux::ecs::Entity right) noexcept
        {
            return entt::to_integral(left) < entt::to_integral(right);
        }

        lux::ecs::RegistryBase* registry_{nullptr};
        bool topology_dirty_{true};
        std::uint64_t topology_revision_{0u};
        std::uint64_t rebuild_count_{0u};
        std::size_t unresolved_count_{0u};

        std::vector<ParentEntry> parent_entries_;
        std::vector<ChildEdge> child_edges_;
        std::vector<lux::ecs::Entity> children_;
        std::vector<ChildRange> child_ranges_;
        std::vector<SubtreeEntry> subtree_ranges_;
        std::vector<std::uint8_t> visit_state_;
        std::vector<lux::ecs::Entity> nodes_;
        std::vector<lux::ecs::Entity> roots_;
        std::vector<lux::ecs::Entity> preorder_;
        std::vector<DfsFrame> dfs_stack_;
    };

    inline void detail_hierarchyIndexOnConstruct(
        lux::ecs::RegistryBase& registry,
        entt::entity)
    {
        registry.ctx().get<HierarchyIndex>().markTopologyDirty();
    }

    inline void detail_hierarchyIndexOnUpdate(
        lux::ecs::RegistryBase& registry,
        entt::entity)
    {
        registry.ctx().get<HierarchyIndex>().markTopologyDirty();
    }

    inline void detail_hierarchyIndexOnDestroy(
        lux::ecs::RegistryBase& registry,
        entt::entity)
    {
        registry.ctx().get<HierarchyIndex>().markTopologyDirty();
    }

    inline void detail_hierarchyMembershipOnDestroy(
        lux::ecs::RegistryBase& registry,
        entt::entity)
    {
        registry.ctx().get<HierarchyIndex>().markTopologyDirty();
    }

    [[nodiscard]] inline HierarchyIndex& hierarchyIndex(
        lux::ecs::RegistryBase& registry)
    {
        return registry.ctx().get<HierarchyIndex>();
    }

    [[nodiscard]] inline const HierarchyIndex& hierarchyIndex(
        const lux::ecs::RegistryBase& registry)
    {
        return registry.ctx().get<HierarchyIndex>();
    }

    inline HierarchyIndex& ensureHierarchyIndex(
        lux::ecs::RegistryBase& registry)
    {
        if (auto* existing = registry.ctx().find<HierarchyIndex>())
            return *existing;
        auto& index = registry.ctx().emplace<HierarchyIndex>(registry);
        static_cast<void>(index.reserveForAdditionalEdges(0u));
        registry.on_construct<ParentComponent>()
            .connect<&detail_hierarchyIndexOnConstruct>();
        registry.on_update<ParentComponent>()
            .connect<&detail_hierarchyIndexOnUpdate>();
        registry.on_destroy<ParentComponent>()
            .connect<&detail_hierarchyIndexOnDestroy>();
        registry.on_destroy<detail::HierarchyMembershipState>()
            .connect<&detail_hierarchyMembershipOnDestroy>();
        for (const auto child : registry.view<const ParentComponent>())
        {
            detail::ensureHierarchyMembership(registry, child);
            const auto parent =
                registry.get<const ParentComponent>(child).parent();
            detail::ensureHierarchyMembership(registry, parent);
        }
        index.markTopologyDirty();
        return index;
    }

    inline lux::ecs::Entity hierarchyRoot(
        const lux::ecs::RegistryBase& registry,
        lux::ecs::Entity entity) noexcept
    {
        const auto* storage = registry.storage<ParentComponent>();
        std::size_t budget = (storage ? storage->size() : 0u) + 1u;
        while (entity != lux::ecs::kNullEntity && budget-- != 0u)
        {
            const auto* parent = registry.try_get<const ParentComponent>(entity);
            if (!parent || !registry.valid(parent->parent()))
                break;
            entity = parent->parent();
        }
        return entity;
    }
} // namespace lux::ecs
