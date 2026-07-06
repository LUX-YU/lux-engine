#pragma once
// ============================================================================
//  HierarchicalTransformSystem.hpp — the shared local→world resolution core.
//
//  d2 and d3 compose a local pose into a world matrix with the SAME algorithm:
//  a value-based-dirty, memoized, parent-first DFS over the neutral
//  HierarchyComponent, with G-07 derived-component maintenance and G-08 cycle
//  breaking. They differ in exactly two axes — the (Local, World) component pair
//  and how a single node's local pose becomes a 4x4 — so those are the only
//  knobs a Policy supplies. Keeping the resolution kernel in ONE place means a
//  cycle/dirty change is made (and reasoned about) once, not mirrored by hand
//  across two kits that must stay behaviourally identical.
//
//  Policy contract:
//    using Local;                                   // ECS local-pose component
//    using World;                                   // ECS derived world component
//    static Eigen::Matrix4f localMatrix(const Local&);   // local pose → 4x4
//    static constexpr const char* kName;            // for the cycle diagnostic
//  Local must expose: bool dirty.
//  World must expose: Eigen::Matrix4f world, prev_world; bool dirty.
// ============================================================================

#include <lux/engine/gameplay/world/systems/ISystem.hpp>
#include <lux/engine/gameplay/world/components/HierarchyComponent.hpp>

#include <Eigen/Core>
#include <cstddef>
#include <cstdio>
#include <unordered_set>
#include <vector>

namespace lux::gameplay
{
    /// Composes each entity's local pose (Policy::Local) into its derived world
    /// component (Policy::World) by walking the HierarchyComponent forest.
    ///
    /// Pass 1 composes every root (no HierarchyComponent) directly from its local
    /// pose. Pass 2 resolves hierarchical entities with a memoized depth-first walk
    /// so a parent is ALWAYS composed before its children — correct for arbitrary
    /// depth, independent of entt's (non-topological) iteration order. The per-entity
    /// world `dirty` is OR-ed with the parent's, so a moving parent marks its whole
    /// subtree dirty (consumers that skip work on !dirty then stay correct).
    ///
    /// Dirty detection is value-based, not flag-based: the freshly-composed world
    /// matrix is compared against the previous frame's, and `dirty` is set whenever
    /// they differ — so any mutation source (game logic, animation, the editor
    /// Inspector, a script) is picked up without teaching the mutator about the flag.
    /// An explicit `Local::dirty` still forces it, OR-ed in.
    template <class Policy>
    class HierarchicalTransformSystem : public lux::gameplay::ISystem
    {
    public:
        using Local = typename Policy::Local;
        using World = typename Policy::World;

        void update(lux::meta::EntityRegistry& registry, float /*dt*/) override
        {
            // G-07: the SYSTEM owns the derived World invariant, so no loader / editor /
            // game code back-fills it — auto-maintain the derived component before
            // composing. After one tick every Local entity has a valid World. (Scene
            // serialization omits World — it is derived, non-persistent — so this is what
            // makes a freshly-loaded scene renderable without an editor-side fixup pass.)
            maintainDerived(registry);

            // Pass 1 — roots (no parent link): world = local pose.
            registry.view<Local, World>(entt::exclude<HierarchyComponent>)
                .each([](Local& tc, World& wc)
            {
                const Eigen::Matrix4f new_world = Policy::localMatrix(tc);
                wc.prev_world = wc.world;
                wc.dirty      = tc.dirty || wc.world != new_world;   // value-based
                wc.world      = new_world;
                tc.dirty      = false;
            });

            // Pass 2 — hierarchical entities, resolved parent-first (memoized DFS, any
            // depth). resolved_ / visiting_ are reused across frames (cleared here) to
            // avoid per-frame allocation.
            resolved_.clear();
            visiting_.clear();
            cycles_last_update_ = 0;
            auto child_view = registry.view<Local, World, HierarchyComponent>();
            for (auto e : child_view)
                resolve(registry, e);

            // G-08: a parent cycle (A→B→A) is a malformed scene. resolve() breaks it by
            // dropping the cycle-closing parent edge to Identity (P1-1), so the cycle nodes
            // reach a STABLE fixed point (fall back to their local pose, no cross-frame
            // drift) instead of composing off a this-frame-incomplete sibling, and counts
            // it. Diagnose in debug so the bad data is fixable; cyclesLastUpdate() exposes
            // it to tests.
#ifndef NDEBUG
            if (cycles_last_update_ > 0)
                std::fprintf(stderr,
                    "[%s] hierarchy cycle detected: %zu back-edge(s) broken this frame; "
                    "cycle nodes fall back to their local transform.\n",
                    Policy::kName, cycles_last_update_);
#endif
        }

        /// Number of hierarchy back-edges (cycles) broken in the last update() — 0 for a
        /// well-formed forest. For diagnostics / tests (G-08).
        [[nodiscard]] std::size_t cyclesLastUpdate() const noexcept { return cycles_last_update_; }

    private:
        /// Keep the derived World in lock-step with its source Local: emplace one on every
        /// Local that lacks it, and drop any World whose source Local was removed (a derived
        /// orphan nothing recomputes — it would otherwise feed a stale matrix to a consumer
        /// whose membership is keyed on World, e.g. a render bridge). Collect first, THEN
        /// mutate: emplacing/removing while iterating the scanned pool is UB.
        void maintainDerived(lux::meta::EntityRegistry& registry)
        {
            scratch_.clear();
            for (auto e : registry.view<Local>(entt::exclude<World>))
                scratch_.push_back(e);
            for (auto e : scratch_)
                registry.emplace<World>(e);

            scratch_.clear();
            for (auto e : registry.view<World>(entt::exclude<Local>))
                scratch_.push_back(e);
            for (auto e : scratch_)
                registry.remove<World>(e);
        }

        /// Ensure @p e's parent is composed before @p e, then compose e's world.
        /// `resolved_` memoizes fully-composed entities (a shared parent / diamond is
        /// visited once); `visiting_` is the current DFS stack, so a re-entry into an entity
        /// still ON the stack is a genuine back-edge (cycle), NOT a diamond — that is broken
        /// + counted (G-08). Every exit AFTER pushing `visiting_` must pop it.
        void resolve(lux::meta::EntityRegistry& registry, lux::meta::entity_id e)
        {
            if (resolved_.count(e)) return;           // already composed this frame
            if (!visiting_.insert(e).second)          // e is on the current DFS stack → cycle
            {
                ++cycles_last_update_;                // G-08: back-edge — don't recurse. e's own
                return;                               // top-level resolve() still composes it, but
            }                                         // its cycle-parent edge is dropped (see below).

            auto* tc = registry.try_get<Local>(e);
            auto* wc = registry.try_get<World>(e);
            if (!tc || !wc) { visiting_.erase(e); return; }  // can't compose — pop + leave as-is
            const auto* hc = registry.try_get<HierarchyComponent>(e);

            Eigen::Matrix4f parent_world = Eigen::Matrix4f::Identity();
            bool            parent_dirty = false;
            if (hc && registry.valid(hc->parent))
            {
                // Roots were composed in Pass 1; only recurse for hierarchical parents
                // (those whose own world still needs composing).
                if (registry.all_of<HierarchyComponent>(hc->parent))
                    resolve(registry, hc->parent);
                // P1-1: if the parent is STILL on the DFS stack after recursing, this edge
                // closes a cycle — the parent's world is this-frame-incomplete. Reading it
                // would mix a stale/fresh sibling matrix and DRIFT every frame
                // (A_n = A_0·(b·a)^n). Skip the parent contribution (leave Identity) so the
                // cycle node composes from its own local pose — a stable fixed point.
                if (!visiting_.count(hc->parent))
                    if (const auto* pwc = registry.try_get<World>(hc->parent))
                    {
                        parent_world = pwc->world;
                        parent_dirty = pwc->dirty;
                    }
            }

            const Eigen::Matrix4f new_world = parent_world * Policy::localMatrix(*tc);
            wc->prev_world = wc->world;
            // Value-based dirty, OR-ed with the parent's dirty so a moving ancestor still
            // drives a re-upload of every descendant.
            wc->dirty      = tc->dirty || parent_dirty || wc->world != new_world;
            wc->world      = new_world;
            tc->dirty      = false;

            visiting_.erase(e);      // pop the DFS stack
            resolved_.insert(e);     // fully composed → memoize
        }

        /// Per-frame memo of already-composed entities (reused; cleared each update).
        std::unordered_set<lux::meta::entity_id> resolved_;
        /// Current DFS recursion stack — distinguishes a back-edge (cycle) from a diamond
        /// (shared parent). Cleared each update.
        std::unordered_set<lux::meta::entity_id> visiting_;
        /// Count of hierarchy back-edges broken in the last update (0 = well-formed).
        std::size_t cycles_last_update_{0};
        /// Reused scratch for the collect-then-mutate derived-component maintenance.
        std::vector<lux::meta::entity_id> scratch_;
    };

} // namespace lux::gameplay
