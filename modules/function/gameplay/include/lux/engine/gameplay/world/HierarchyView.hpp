#pragma once
// ============================================================================
//  HierarchyView.hpp — read-only traversal helpers over HierarchyComponent.
//
//  The scene hierarchy is stored sparsely: only a CHILD carries a
//  HierarchyComponent (pointing at its parent); a root has none. Several
//  consumers need the inverse (parent -> children) or the root of an entity,
//  each with the same cycle-safety contract. This centralises both so that
//  contract lives in exactly one place.
// ============================================================================

#include <lux/engine/gameplay/world/components/HierarchyComponent.hpp>

#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lux::gameplay
{
    /**
     * @brief A snapshot of the scene's parent -> children structure.
     *
     *        Built once from every HierarchyComponent, then queried cheaply. The
     *        editor builds one per frame to render the Hierarchy tree and to
     *        highlight a whole object (root + all descendants).
     *
     *        Subtree walks are cycle/diamond-safe — a visited-set terminates a
     *        malformed parent cycle rather than recursing forever, matching the
     *        TransformSystem's contract.
     */
    class HierarchyView
    {
    public:
        explicit HierarchyView(const entt::registry& registry)
        {
            for (const auto child : registry.view<const HierarchyComponent>())
            {
                const auto& hc = registry.get<const HierarchyComponent>(child);
                if (registry.valid(hc.parent))
                    children_of_[hc.parent].push_back(child);
            }
        }

        /// Direct children of @p entity (empty if it is a leaf / has none).
        std::span<const lux::meta::entity_id>
        childrenOf(lux::meta::entity_id entity) const noexcept
        {
            if (const auto it = children_of_.find(entity); it != children_of_.end())
                return it->second;
            return {};
        }

        /// True if @p entity has at least one valid child.
        bool hasChildren(lux::meta::entity_id entity) const noexcept
        {
            const auto it = children_of_.find(entity);
            return it != children_of_.end() && !it->second.empty();
        }

        /**
         * @brief Visit @p root and every descendant exactly once, root first.
         * @param visit  Callable `void(lux::meta::entity_id)`.
         */
        template <class Visit>
        void forEachInSubtree(lux::meta::entity_id root, Visit&& visit) const
        {
            std::unordered_set<lux::meta::entity_id> seen;
            walk(root, seen, visit);
        }

    private:
        template <class Visit>
        void walk(lux::meta::entity_id                       entity,
                  std::unordered_set<lux::meta::entity_id>&  seen,
                  Visit&                                     visit) const
        {
            if (!seen.insert(entity).second)   // cycle / diamond guard
                return;
            visit(entity);
            if (const auto it = children_of_.find(entity); it != children_of_.end())
                for (const auto child : it->second)
                    walk(child, seen, visit);
        }

        std::unordered_map<lux::meta::entity_id,
                           std::vector<lux::meta::entity_id>> children_of_;
    };

    /**
     * @brief Walk parent links from @p entity up to its top-level root — the
     *        ancestor with no valid parent. Needs no children map.
     *
     *        Promotes a viewport pick (which may land on a child mesh of a
     *        multi-mesh model) to the whole object's root. Cycle-safe: stops at
     *        the first entity whose parent is invalid (a root carries no
     *        HierarchyComponent, so its `try_get` is null).
     */
    inline lux::meta::entity_id
    hierarchyRoot(const entt::registry& registry, lux::meta::entity_id entity) noexcept
    {
        while (entity != lux::meta::null_entity)
        {
            const auto* hc = registry.try_get<const HierarchyComponent>(entity);
            if (hc == nullptr || !registry.valid(hc->parent))
                break;
            entity = hc->parent;
        }
        return entity;
    }

} // namespace lux::gameplay
