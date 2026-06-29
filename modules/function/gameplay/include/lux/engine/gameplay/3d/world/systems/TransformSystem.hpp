#pragma once
#include <lux/engine/gameplay/world/systems/ISystem.hpp>
#include "../components/TransformComponent.hpp"
#include "../components/WorldTransformComponent.hpp"
#include <lux/engine/gameplay/world/components/HierarchyComponent.hpp>
#include <Eigen/Geometry>
#include <unordered_set>

namespace lux::gameplay::d3
{
    using lux::gameplay::HierarchyComponent;   // parent link — stays in the gameplay core

    /// Converts each entity's TransformComponent into a WorldTransformComponent
    /// by composing TRS matrices and propagating through the parent hierarchy.
    ///
    /// Pass 1 composes every root (no HierarchyComponent) directly from its
    /// local TRS. Pass 2 resolves hierarchical entities with a memoized
    /// depth-first walk so a parent is ALWAYS composed before its children —
    /// correct for arbitrary depth, independent of entt's (non-topological)
    /// iteration order. The per-entity world `dirty` flag is OR-ed with the
    /// parent's, so a moving parent marks its whole subtree dirty (consumers
    /// that skip work on !dirty then stay correct).
    ///
    /// Dirty detection is **value-based**, not flag-based: the freshly-composed
    /// world matrix is compared bit-for-bit against the previous frame's
    /// world matrix, and `wc.dirty` is set whenever they differ. This means
    /// any source that mutates the local TRS — game logic, animation, the
    /// editor Inspector, a script — is automatically picked up without
    /// having to teach the mutator about the dirty flag. Code paths that
    /// already set `tc.dirty = true` (e.g. to force a re-upload after a
    /// resource swap) still work, OR-ed in.
    class TransformSystem final : public lux::gameplay::ISystem
    {
    public:
        void update(lux::meta::EntityRegistry& registry, float /*dt*/) override
        {
            // ----------------------------------------------------------------
            // Pass 1 — roots (no parent link): world = local TRS.
            // ----------------------------------------------------------------
            registry.view<TransformComponent, WorldTransformComponent>(
                entt::exclude<HierarchyComponent>)
                .each([](TransformComponent& tc, WorldTransformComponent& wc)
            {
                const Eigen::Matrix4f new_world = computeTRS(tc);
                wc.prev_world = wc.world;
                // Value-based dirty: detect Inspector / scripting / animation
                // edits that bypass the explicit `tc.dirty` flag.
                wc.dirty      = tc.dirty || wc.world != new_world;
                wc.world      = new_world;
                tc.dirty      = false;
            });

            // ----------------------------------------------------------------
            // Pass 2 — hierarchical entities, resolved parent-first (memoized
            // DFS, any depth). resolved_ is reused across frames (cleared here)
            // to avoid per-frame allocation.
            // ----------------------------------------------------------------
            resolved_.clear();
            auto child_view = registry.view<TransformComponent, WorldTransformComponent,
                                            HierarchyComponent>();
            for (auto e : child_view)
                resolve(registry, e);
        }

    private:
        /// Ensure @p e's parent is composed before @p e, then compose e's world.
        /// Marks e resolved BEFORE recursing so a malformed parent cycle still
        /// terminates (the back-edge entity keeps its previous-frame world).
        void resolve(lux::meta::EntityRegistry& registry, lux::meta::entity_id e)
        {
            if (!resolved_.insert(e).second) return;  // already resolved this frame

            auto* tc = registry.try_get<TransformComponent>(e);
            auto* wc = registry.try_get<WorldTransformComponent>(e);
            if (!tc || !wc) return;  // can't compose without both — leave as-is
            const auto* hc = registry.try_get<HierarchyComponent>(e);

            Eigen::Matrix4f parent_world = Eigen::Matrix4f::Identity();
            bool            parent_dirty = false;
            if (hc && registry.valid(hc->parent))
            {
                // Roots were composed in Pass 1; only recurse for hierarchical
                // parents (those whose own world still needs composing).
                if (registry.all_of<HierarchyComponent>(hc->parent))
                    resolve(registry, hc->parent);
                if (const auto* pwc = registry.try_get<WorldTransformComponent>(hc->parent))
                {
                    parent_world = pwc->world;
                    parent_dirty = pwc->dirty;
                }
            }

            const Eigen::Matrix4f new_world = parent_world * computeTRS(*tc);
            wc->prev_world = wc->world;
            // Value-based dirty, OR-ed with the parent's dirty so a moving
            // ancestor still drives a re-upload of every descendant.
            wc->dirty      = tc->dirty || parent_dirty || wc->world != new_world;
            wc->world      = new_world;
            tc->dirty      = false;
        }

        static Eigen::Matrix4f computeTRS(const TransformComponent& tc)
        {
            Eigen::Affine3f t = Eigen::Affine3f::Identity();
            t.translate(tc.position);
            t.rotate(tc.rotation);
            t.scale(tc.scale);
            return t.matrix();
        }

        /// Per-frame memo of already-composed entities (reused; cleared each update).
        std::unordered_set<lux::meta::entity_id> resolved_;
    };

} // namespace lux::gameplay::d3
