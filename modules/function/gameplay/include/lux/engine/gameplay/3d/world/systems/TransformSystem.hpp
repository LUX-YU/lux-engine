#pragma once
#include <lux/engine/gameplay/world/systems/ISystem.hpp>
#include "../components/TransformComponent.hpp"
#include "../components/WorldTransformComponent.hpp"
#include <lux/engine/gameplay/world/components/HierarchyComponent.hpp>
#include <Eigen/Geometry>
#include <unordered_set>
#include <vector>
#include <cstddef>
#include <cstdio>

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
            // G-07 — the SYSTEM owns the WorldTransform invariant, so no loader /
            // editor / game code back-fills it: auto-maintain the derived component
            // before composing. After one tick every TransformComponent entity has a
            // valid WorldTransform. (Scene serialization omits WorldTransform — it is
            // derived, non-persistent — so this is what makes a freshly-loaded scene
            // renderable without an editor-side fixup pass.)
            // ----------------------------------------------------------------
            maintainDerived(registry);

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
            visiting_.clear();
            cycles_last_update_ = 0;
            auto child_view = registry.view<TransformComponent, WorldTransformComponent,
                                            HierarchyComponent>();
            for (auto e : child_view)
                resolve(registry, e);

            // G-08: a parent cycle (A→B→A) is a malformed scene. resolve() breaks it
            // (a back-edge does not recurse; the entity keeps its previous-frame world,
            // so descendants can't mix a half-updated matrix) and counts it. Diagnose in
            // debug so the bad data is fixable; `cyclesLastUpdate()` exposes it to tests.
#ifndef NDEBUG
            if (cycles_last_update_ > 0)
                std::fprintf(stderr,
                    "[TransformSystem] hierarchy cycle detected: %zu back-edge(s) broken "
                    "this frame; affected entities keep their previous-frame world.\n",
                    cycles_last_update_);
#endif
        }

    public:
        /// Number of hierarchy back-edges (cycles) broken in the last update() — 0 for a
        /// well-formed forest. For diagnostics / tests (G-08).
        [[nodiscard]] std::size_t cyclesLastUpdate() const noexcept { return cycles_last_update_; }

    private:
        /// Keep the derived WorldTransform in lock-step with its source Transform:
        /// emplace one on every TransformComponent that lacks it, and drop any
        /// WorldTransform whose source Transform was removed (a derived orphan nothing
        /// recomputes — it would otherwise feed a stale matrix to a consumer whose
        /// membership is keyed on WorldTransform, e.g. the mesh bridge). Collect first,
        /// THEN mutate: emplacing/removing while iterating the scanned pool is UB.
        void maintainDerived(lux::meta::EntityRegistry& registry)
        {
            scratch_.clear();
            for (auto e : registry.view<TransformComponent>(entt::exclude<WorldTransformComponent>))
                scratch_.push_back(e);
            for (auto e : scratch_)
                registry.emplace<WorldTransformComponent>(e);

            scratch_.clear();
            for (auto e : registry.view<WorldTransformComponent>(entt::exclude<TransformComponent>))
                scratch_.push_back(e);
            for (auto e : scratch_)
                registry.remove<WorldTransformComponent>(e);
        }

        /// Ensure @p e's parent is composed before @p e, then compose e's world.
        /// `resolved_` memoizes fully-composed entities (a shared parent / diamond is
        /// visited once); `visiting_` is the current DFS stack, so a re-entry into an
        /// entity still ON the stack is a genuine back-edge (cycle), NOT a diamond —
        /// that is broken + counted (G-08). Every exit AFTER pushing `visiting_` must
        /// pop it.
        void resolve(lux::meta::EntityRegistry& registry, lux::meta::entity_id e)
        {
            if (resolved_.count(e)) return;           // already composed this frame
            if (!visiting_.insert(e).second)          // e is on the current DFS stack → cycle
            {
                ++cycles_last_update_;                // G-08: break the back-edge (don't recurse)
                return;                               // e keeps its previous-frame world
            }

            auto* tc = registry.try_get<TransformComponent>(e);
            auto* wc = registry.try_get<WorldTransformComponent>(e);
            if (!tc || !wc) { visiting_.erase(e); return; }  // can't compose — pop + leave as-is
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

            visiting_.erase(e);      // pop the DFS stack
            resolved_.insert(e);     // fully composed → memoize
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
        /// Current DFS recursion stack — distinguishes a back-edge (cycle) from a
        /// diamond (shared parent). Cleared each update.
        std::unordered_set<lux::meta::entity_id> visiting_;
        /// Count of hierarchy back-edges broken in the last update (0 = well-formed).
        std::size_t cycles_last_update_{0};
        /// Reused scratch for the collect-then-mutate derived-component maintenance.
        std::vector<lux::meta::entity_id> scratch_;
    };

} // namespace lux::gameplay::d3
