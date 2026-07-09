#pragma once
// ============================================================================
//  HierarchicalTransformSystem.hpp — the shared local→world resolution core.
//
//  d2 and d3 compose a local pose into a world matrix with the SAME algorithm;
//  they differ in exactly two axes — the (Local, World) component pair and how a
//  single node's local pose becomes a 4x4 — so those are the only knobs a Policy
//  supplies. Keeping the resolution kernel in ONE place means an ordering/dirty
//  change is made (and reasoned about) once, not mirrored by hand across two
//  kits that must stay behaviourally identical.
//
//  MODEL: SYSTEM-OWNED PREORDER VECTOR + one linear pass + a value-based skip
//  gate. The traversal order (every parent visited BEFORE its children) is
//  derived data held in an internal vector — the component pool is NEVER
//  reordered (no registry.sort: nothing fights a future owning group, other
//  consumers see whatever pool order entt keeps, and rebuilds move no
//  components). The order is rebuilt only when the structure actually changed:
//
//    per frame   O(N) structure scan (packed (entity,parent) pairs vs snapshot
//                — a glorified memcmp; catches DIRECT field writes, which fire
//                no entt signal) + O(N) walk of the preorder vector where an
//                UNCHANGED entity costs a handful of compares and ZERO writes;
//                only entities whose compose inputs changed pay matrix math,
//                so the arithmetic cost is O(K changed), not O(N).
//    spawn       a pure pool APPEND takes a fast path: a new entity whose
//                parent is already ordered is pushed onto the vector's tail —
//                ancestors-first holds by construction, nothing is rebuilt.
//                Batches over kMaxAppendBatch (bulk scene loads) rebuild
//                instead, keeping whole-scene orders canonical.
//    on change   rebuild: bucket children, deterministic DFS pre-order
//                numbering (roots and siblings by entity id — pool churn can
//                never change the outcome), new snapshot. Per-entity RESULTS
//                are order-independent either way (a world matrix depends only
//                on the ancestor chain), so the append tail affects visiting
//                order, never values.
//
//  DIRTY/SKIP is value-based, not trust-based: the gate compares the ACTUAL
//  compose inputs — the local pose (Policy::poseEquals) and the parent entity
//  whose world was consumed (World::last_parent) — against what the current
//  world was composed from. Any mutation source (game logic, animation, the
//  editor Inspector writing fields directly, a script) is picked up with no
//  flag discipline required; an explicit Local::dirty still forces, OR-ed in.
//  A skipped entity writes NOTHING — world/prev_world/dirty are already
//  consistent — so a static scene's pass is pure reads. The gate also requires
//  !World::dirty, which costs exactly one extra recompose on the frame AFTER a
//  change: that recompose is what settles prev_world (= world) and dirty
//  (= false), buying the zero-write steady state without a special skip-path
//  write. Structure changes self-invalidate at the VALUE level too: reparent,
//  parent destroyed, parent's World appearing/vanishing all flip the effective
//  parent identity, which last_parent catches with one id compare — no cache
//  wipe on rebuild needed.
//
//  CYCLES are handled at the EDGES, not per frame (2026-07-06 ruling: entries
//  validate, the runtime does not pay for malformed data):
//    - a cycle cannot make this system hang or recurse — structurally: the
//      rebuild DFS walks derived child buckets (each node visited at most once)
//      and the frame pass is a bounded linear loop;
//    - entities whose parent chain never reaches a root (i.e. on or under a
//      cycle) simply get NO order — they never enter the preorder vector, the
//      pass never sees them, unresolvedCount() exposes the count (+ a debug
//      diagnostic per rebuild);
//    - the ENTRY POINTS own prevention/repair: wouldCreateHierarchyCycle() for
//      reparent UIs/APIs, validateHierarchy()/repairHierarchyCycles() for scene
//      load (Scene::load repairs + warns).
//
//  Policy contract:
//    using Local;                                   // ECS local-pose component
//    using World;                                   // ECS derived world component
//    static Eigen::Matrix4f localMatrix(const Local&);       // local pose → 4x4
//    static bool poseEquals(const Local&, const Local&);     // pose fields only,
//                                                            // EXACT equality
//    static constexpr const char* kName;            // for diagnostics
//  Local must expose: bool dirty.
//  World must expose: Eigen::Matrix4f world, prev_world; bool dirty;
//                     Local last_local; lux::meta::entity_id last_parent
//                     (system-owned skip-gate scratch, see the components).
//
//  A useful property of the pre-order (for FUTURE subtree operations — culling,
//  subtree hide): a subtree occupies a CONTIGUOUS range of the preorder vector
//  (appends only ever extend a subtree's tail while it stays the vector's last).
//  No API is exposed until a consumer exists; deriving {first, count} per entity
//  is a few lines in rebuildOrder() when one does.
// ============================================================================

#include <lux/engine/gameplay/world/systems/ISystem.hpp>
#include <lux/engine/gameplay/world/components/HierarchyComponent.hpp>

#include <Eigen/Core>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace lux::gameplay
{
    // ── G-07 derived-component maintenance (SIGNAL-DRIVEN) ─────────────────────
    //  Every system that owns a derived component keyed on a source component
    //  (Transform→WorldTransform here, Camera2D→Camera2DCache in the 2D kit) must
    //  hold the invariant "every Source carries a Derived; no Derived outlives its
    //  Source" (a derived orphan nothing recomputes would feed stale data to a
    //  consumer whose membership is keyed on Derived, e.g. a render bridge).
    //
    //  This used to be a per-frame O(N) DOUBLE view-scan. It is now maintained in
    //  O(1) per structural change via entt construction/destruction signals — the
    //  per-frame cost drops to ZERO. Why these signals and not on_update: component
    //  construct/destroy ALWAYS routes through the registry, so on_construct /
    //  on_destroy are total; on_update, by contrast, is bypassed by a raw
    //  `registry.get<T>(e).field = ...` write (exactly what the editor Inspector and
    //  reflection-based deserialization do) — trusting it would silently miss them.
    //  Membership (does an entity HAVE Source) is a structural fact those raw writes
    //  cannot forge, so the trust surface the per-frame value-scan guards elsewhere
    //  does not exist here: this maintenance is safe to make signal-driven.
    //
    //  The listeners are STATELESS free functions, so there is no lifetime coupling
    //  to the system: a connection dies with its registry, and a stale call (system
    //  destroyed while the registry lives) would at worst do harmless maintenance no
    //  one reads — never a dangling `this`. Callback type is entt's listener shape
    //  `void(entt::registry&, entt::entity)`: entt passes the base-registry subobject
    //  of EntityRegistry, so the parameter is the base, not the lux subclass.
    namespace detail
    {
        template <class Source, class Derived>
        void emplaceDerivedOnSourceConstruct(entt::registry& r, entt::entity e)
        {
            // Signals fire exactly once per construction; the guard only prevents
            // clobbering a Derived a consumer already placed (never a normal path).
            if (!r.all_of<Derived>(e))
                r.emplace<Derived>(e);
        }

        template <class Source, class Derived>
        void removeDerivedOnSourceDestroy(entt::registry& r, entt::entity e)
        {
            // remove<> is a no-op when absent — safe if a full-entity destroy already
            // reaped Derived earlier in its component cascade.
            r.remove<Derived>(e);
        }
    } // namespace detail

    /// Wire the two maintenance signals AND backfill once. Called a SINGLE time per
    /// system (the caller guards with a bool), on the first update — the first point
    /// a system holds its registry (ISystem has no attach hook). The backfill is
    /// required because signals only fire for FUTURE construct/destroy, while a scene
    /// may already hold Source entities (loaded before the system installed).
    /// @p scratch is caller-owned so its capacity is reused (used only by backfill).
    /// Assumes one system instance per registry (the install() 1:1 model).
    template <class Source, class Derived>
    inline void connectDerivedMaintenance(lux::meta::EntityRegistry& registry,
                                          std::vector<lux::meta::entity_id>& scratch)
    {
        registry.on_construct<Source>()
            .template connect<&detail::emplaceDerivedOnSourceConstruct<Source, Derived>>();
        registry.on_destroy<Source>()
            .template connect<&detail::removeDerivedOnSourceDestroy<Source, Derived>>();

        // One-time backfill. Collect first, THEN mutate: emplacing / removing the
        // EXCLUDED pool while iterating the view invalidates it (UB) — the same
        // subtlety the old per-frame scan carried, now paid exactly once.
        scratch.clear();
        for (auto e : registry.view<Source>(entt::exclude<Derived>))
            scratch.push_back(e);
        for (auto e : scratch)
            registry.emplace<Derived>(e);

        scratch.clear();
        for (auto e : registry.view<Derived>(entt::exclude<Source>))
            scratch.push_back(e);
        for (auto e : scratch)
            registry.remove<Derived>(e);
    }

    // ── Hierarchy validation utilities (the ENTRY-POINT guards) ────────────────
    //  Prevention and repair live at the data's entry points (reparent UI/API,
    //  scene load) — the per-frame system tolerates but never pays for cycles.

    /// Would parenting @p child under @p new_parent close a cycle? O(depth) walk
    /// up @p new_parent's ancestor chain. For reparent UIs / gameplay setParent
    /// APIs to call BEFORE writing HierarchyComponent::parent.
    [[nodiscard]] inline bool wouldCreateHierarchyCycle(lux::meta::EntityRegistry& registry,
                                                        lux::meta::entity_id child,
                                                        lux::meta::entity_id new_parent)
    {
        // A self-parent is a cycle; walking is bounded by the pool size so even a
        // PRE-EXISTING cycle upstream cannot hang the check.
        std::size_t budget = registry.storage<HierarchyComponent>().size() + 1;
        for (auto e = new_parent; registry.valid(e) && budget != 0; --budget)
        {
            if (e == child) return true;
            const auto* hc = registry.try_get<HierarchyComponent>(e);
            if (!hc) break;
            e = hc->parent;
        }
        return budget == 0;   // ran out of budget → an upstream cycle exists anyway
    }

    namespace detail
    {
        /// Three-colour parent-chain walk over every hierarchy entity. Entities ON a
        /// cycle's loop are appended to @p cores (when non-null). Returns the number
        /// of core entities found. O(N) amortized — each entity is finalized once.
        inline std::size_t collectHierarchyCycleCores(lux::meta::EntityRegistry& registry,
                                                      std::vector<lux::meta::entity_id>* cores)
        {
            auto& st = registry.storage<HierarchyComponent>();
            std::size_t max_idx = 0;
            for (std::size_t i = 0; i < st.size(); ++i)
                max_idx = std::max<std::size_t>(max_idx, entt::to_entity(st.data()[i]));
            std::vector<std::uint8_t> colour(max_idx + 1, 0);   // 0 white, 1 gray, 2 black
            std::vector<lux::meta::entity_id> stack;

            std::size_t core_count = 0;
            for (std::size_t i = 0; i < st.size(); ++i)
            {
                auto e = st.data()[i];
                if (colour[entt::to_entity(e)] != 0) continue;

                stack.clear();
                // Walk up the chain, graying as we go, until we terminate or bite our
                // own gray tail.
                while (true)
                {
                    colour[entt::to_entity(e)] = 1;
                    stack.push_back(e);
                    const auto parent = st.contains(e) ? st.get(e).parent : lux::meta::null_entity;
                    if (!registry.valid(parent) || !st.contains(parent))
                        break;                                        // terminates at a root
                    const std::uint8_t pc = colour[entt::to_entity(parent)];
                    if (pc == 2) break;                               // joins an already-good chain
                    if (pc == 1)                                      // gray → THIS walk's stack: a loop
                    {
                        // The loop = the stack segment from `parent` to the top.
                        for (std::size_t s = stack.size(); s-- != 0;)
                        {
                            ++core_count;
                            if (cores) cores->push_back(stack[s]);
                            if (stack[s] == parent) break;
                        }
                        break;
                    }
                    e = parent;
                }
                for (auto walked : stack)
                    colour[entt::to_entity(walked)] = 2;              // finalize
            }
            return core_count;
        }
    } // namespace detail

    /// Count the entities sitting ON a parent cycle (0 = well-formed). For load-time
    /// validation and tests; the per-frame system independently reports the affected
    /// set via unresolvedCount().
    [[nodiscard]] inline std::size_t validateHierarchy(lux::meta::EntityRegistry& registry)
    {
        return detail::collectHierarchyCycleCores(registry, nullptr);
    }

    /// Repair: strip HierarchyComponent from every entity ON a cycle's loop — they
    /// become roots (visible, selectable, re-parentable), and their subtrees reattach
    /// through them automatically. Returns how many links were removed. Intended for
    /// scene load ("repair + warn" beats rejecting a whole file for one bad link).
    inline std::size_t repairHierarchyCycles(lux::meta::EntityRegistry& registry)
    {
        std::vector<lux::meta::entity_id> cores;
        detail::collectHierarchyCycleCores(registry, &cores);
        for (auto e : cores)
            registry.remove<HierarchyComponent>(e);
        return cores.size();
    }

    /// Composes each entity's local pose (Policy::Local) into its derived world
    /// component (Policy::World).
    ///
    /// Pass 1 composes every root (no HierarchyComponent) directly from its local
    /// pose. Pass 2 walks the system-owned preorder vector linearly — every parent
    /// precedes its children, so each entity composes exactly once with its parent's
    /// world already fresh. The parent link is read LIVE each visit, so an
    /// order-preserving reparent is correct even before the (conservative) rebuild.
    ///
    /// Both passes go through the value-based skip gate (see the header comment):
    /// unchanged compose inputs → zero writes; dirty stays exact (set iff the world
    /// VALUE changed, or forced by Local::dirty / a dirty parent).
    template <class Policy>
    class HierarchicalTransformSystem : public lux::gameplay::ISystem
    {
    public:
        using Local = typename Policy::Local;
        using World = typename Policy::World;

        void update(lux::meta::EntityRegistry& registry, float /*dt*/) override
        {
            // G-07: the SYSTEM owns the derived World invariant. Wire it to entt
            // construct/destroy signals ONCE (first update = first sight of the
            // registry) + a one-time backfill of any pre-existing Source entities;
            // thereafter the invariant is maintained at O(1) per structural change,
            // zero per-frame cost. A freshly-loaded scene is still renderable with no
            // editor-side fixup (scene serialization omits World).
            if (!derived_maintenance_connected_)
            {
                connectDerivedMaintenance<Local, World>(registry, scratch_);
                derived_maintenance_connected_ = true;
#ifndef NDEBUG
                maintenance_registry_ = &registry;
#endif
            }
#ifndef NDEBUG
            assert(maintenance_registry_ == &registry &&
                   "HierarchicalTransformSystem reused across registries: its G-07 "
                   "signal maintenance is wired to a different one");
#endif

            // Pass 1 — roots (no parent link): world = local pose.
            registry.view<Local, World>(entt::exclude<HierarchyComponent>)
                .each([](Local& tc, World& wc)
            {
                if (!tc.dirty && !wc.dirty &&
                    wc.last_parent == lux::meta::null_entity &&
                    Policy::poseEquals(tc, wc.last_local))
                    return;   // skip gate: inputs unchanged → zero writes

                const Eigen::Matrix4f new_world = Policy::localMatrix(tc);
                wc.prev_world  = wc.world;
                wc.dirty       = tc.dirty || wc.world != new_world;   // value-based
                wc.world       = new_world;
                wc.last_local  = tc;
                wc.last_parent = lux::meta::null_entity;
                tc.dirty       = false;
            });

            auto& st = registry.storage<HierarchyComponent>();
            if (st.empty())
            {
                if (!structure_cache_.empty())
                {   // pool fully drained: drop the stale order bookkeeping
                    structure_cache_.clear();
                    preorder_.clear();
                    unresolved_count_ = 0;
                }
                return;
            }

            // Re-derive the topological order ONLY when the structure changed. The
            // scan is a linear compare of packed (entity, parent) pairs against the
            // last snapshot — it catches parent-field rewrites that fire no signal;
            // pool add/remove shows up as a size/prefix mismatch. A pure append
            // (spawn) extends the order instead of rebuilding it.
            switch (classifyStructureDelta(st))
            {
            case EStructureDelta::None:
                break;
            case EStructureDelta::Appended:
                if (tryAppendFastPath(registry, st))
                    break;
                [[fallthrough]];   // chained under an unresolved tail / oversized batch
            case EStructureDelta::Changed:
                rebuildOrder(registry, st);
                break;
            }

            // Pass 2 — ONE linear walk, parents first by construction. Cycle-involved
            // entities are not in the vector at all.
            for (const auto e : preorder_)
            {
                auto* tc = registry.try_get<Local>(e);
                auto* wc = registry.try_get<World>(e);
                if (!tc || !wc)
                    continue;   // a grouping node without a pose computes nothing

                // Effective parent = the entity whose World actually feeds this
                // compose (live read). Parent destroyed / World removed / gained all
                // flip this id, which the gate catches — no cache invalidation pass.
                const Eigen::Matrix4f* parent_world = nullptr;
                bool                   parent_dirty = false;
                lux::meta::entity_id   effective    = lux::meta::null_entity;
                const auto parent = st.get(e).parent;
                if (registry.valid(parent))
                    if (const auto* pwc = registry.try_get<World>(parent))
                    {
                        parent_world = &pwc->world;   // fresh: pass 1 or earlier in this walk
                        parent_dirty = pwc->dirty;
                        effective    = parent;
                    }

                if (!tc->dirty && !wc->dirty && !parent_dirty &&
                    effective == wc->last_parent &&
                    Policy::poseEquals(*tc, wc->last_local))
                    continue;   // skip gate: inputs unchanged → zero writes

                Eigen::Matrix4f new_world = Policy::localMatrix(*tc);
                if (parent_world)
                    new_world = *parent_world * new_world;
                wc->prev_world  = wc->world;
                wc->dirty       = tc->dirty || parent_dirty || wc->world != new_world;
                wc->world       = new_world;
                wc->last_local  = *tc;
                wc->last_parent = effective;
                tc->dirty       = false;
            }
        }

        /// Entities excluded from the last ordering because their parent chain never
        /// reaches a root (on or under a cycle). 0 for well-formed scenes; the entry
        /// guards (wouldCreateHierarchyCycle / repairHierarchyCycles) are the fix.
        [[nodiscard]] std::size_t unresolvedCount() const noexcept { return unresolved_count_; }

    private:
        using Storage = decltype(std::declval<lux::meta::EntityRegistry&>()
                                     .storage<HierarchyComponent>());

        enum class EStructureDelta : std::uint8_t { None, Appended, Changed };

        [[nodiscard]] EStructureDelta classifyStructureDelta(
            std::remove_reference_t<Storage>& st) const
        {
            if (!order_valid_ || st.size() < structure_cache_.size())
                return EStructureDelta::Changed;
            const auto* packed = st.data();
            for (std::size_t i = 0; i < structure_cache_.size(); ++i)
            {
                const auto e = packed[i];
                if (structure_cache_[i].first  != entt::to_integral(e) ||
                    structure_cache_[i].second != entt::to_integral(st.get(e).parent))
                    return EStructureDelta::Changed;
            }
            return st.size() == structure_cache_.size() ? EStructureDelta::None
                                                        : EStructureDelta::Appended;
        }

        /// Spawn fast path: the only delta is pool APPENDS (entt emplaces at the
        /// packed tail), so each new entity whose parent is already ordered — or is
        /// a forest root — is appended to the preorder tail: ancestors-first holds
        /// by construction, nothing is rebuilt or moved. A fixpoint loop absorbs
        /// batches whose pool order is child-before-parent (Hierarchy emplaced on
        /// the child first). Returns false — full rebuild — for oversized batches
        /// (bulk loads keep the canonical deterministic order) and for anything
        /// chaining to an unresolved/cyclic tail.
        [[nodiscard]] bool tryAppendFastPath(lux::meta::EntityRegistry& registry,
                                             std::remove_reference_t<Storage>& st)
        {
            const std::size_t old_n = structure_cache_.size();
            const std::size_t n     = st.size();
            if (n - old_n > kMaxAppendBatch)
                return false;

            pending_appends_.clear();
            std::size_t max_idx = 0;
            for (std::size_t i = old_n; i < n; ++i)
            {
                const auto e = st.data()[i];
                max_idx = std::max<std::size_t>(max_idx, entt::to_entity(e));
                pending_appends_.push_back(e);
            }
            if (order_.size() <= max_idx)
                order_.resize(max_idx + 1, kUnresolvedOrder);
            // Reset FIRST: a recycled entity index may still carry a dead entity's
            // rank — trusting it could order a child before its (new) parent.
            for (const auto e : pending_appends_)
                order_[entt::to_entity(e)] = kUnresolvedOrder;

            std::size_t remaining = pending_appends_.size();
            for (bool progress = true; remaining != 0 && progress;)
            {
                progress = false;
                for (auto& e : pending_appends_)
                {
                    if (e == lux::meta::null_entity)
                        continue;   // already placed
                    const auto parent   = st.get(e).parent;
                    const bool has_edge = registry.valid(parent) && st.contains(parent);
                    if (has_edge && order_[entt::to_entity(parent)] == kUnresolvedOrder)
                        continue;   // parent not placed yet (maybe later this loop)
                    order_[entt::to_entity(e)] = static_cast<std::uint32_t>(preorder_.size());
                    preorder_.push_back(e);
                    e = lux::meta::null_entity;
                    --remaining;
                    progress = true;
                }
            }
            if (remaining != 0)
                return false;   // partial order_/preorder_ writes: rebuild resets them

            for (std::size_t i = old_n; i < n; ++i)
            {
                const auto e = st.data()[i];
                structure_cache_.emplace_back(entt::to_integral(e),
                                              entt::to_integral(st.get(e).parent));
            }
            return true;
        }

        void rebuildOrder(lux::meta::EntityRegistry& registry,
                          std::remove_reference_t<Storage>& st)
        {
            const std::size_t n = st.size();

            // Split the pool into forest roots (parent is invalid or not itself
            // hierarchical — its world is composed by pass 1 / read live) and
            // parent→child edges. Both are sorted by entity id, so the resulting
            // order is DETERMINISTIC no matter what pool order the churn left behind.
            roots_.clear();
            edges_.clear();
            std::size_t max_idx = 0;
            for (std::size_t i = 0; i < n; ++i)
            {
                const auto e = st.data()[i];
                max_idx = std::max<std::size_t>(max_idx, entt::to_entity(e));
                const auto parent = st.get(e).parent;
                if (registry.valid(parent) && st.contains(parent))
                    edges_.emplace_back(entt::to_integral(parent), e);
                else
                    roots_.push_back(e);
            }
            std::sort(roots_.begin(), roots_.end(),
                      [](auto a, auto b) { return entt::to_integral(a) < entt::to_integral(b); });
            std::sort(edges_.begin(), edges_.end(),
                      [](const auto& a, const auto& b) {
                          return a.first != b.first
                                     ? a.first < b.first
                                     : entt::to_integral(a.second) < entt::to_integral(b.second);
                      });

            if (order_.size() <= max_idx)
                order_.resize(max_idx + 1, kUnresolvedOrder);
            for (std::size_t i = 0; i < n; ++i)
                order_[entt::to_entity(st.data()[i])] = kUnresolvedOrder;

            // Deterministic pre-order numbering via an explicit stack (never recurses,
            // never revisits: each node is pushed exactly once, from its parent's edge
            // bucket). Children are pushed in REVERSE id order so they POP ascending.
            // The numbering IS the traversal vector — the pool itself is not touched.
            preorder_.clear();
            preorder_.reserve(n);
            dfs_stack_.assign(roots_.rbegin(), roots_.rend());
            while (!dfs_stack_.empty())
            {
                const auto e = dfs_stack_.back();
                dfs_stack_.pop_back();
                order_[entt::to_entity(e)] = static_cast<std::uint32_t>(preorder_.size());
                preorder_.push_back(e);

                const auto key   = entt::to_integral(e);
                auto       first = std::lower_bound(edges_.begin(), edges_.end(), key,
                                        [](const auto& edge, auto k) { return edge.first < k; });
                auto       last  = first;
                while (last != edges_.end() && last->first == key) ++last;
                for (auto it = std::make_reverse_iterator(last);
                     it != std::make_reverse_iterator(first); ++it)
                    dfs_stack_.push_back(it->second);
            }

            // Whatever was never reached sits on/under a parent cycle: it keeps
            // kUnresolvedOrder, never enters the vector, and the pass never sees it.
            // The entry guards are the fix; here we only diagnose.
            unresolved_count_ = n - preorder_.size();
#ifndef NDEBUG
            if (unresolved_count_ > 0)
                std::fprintf(stderr,
                    "[%s] hierarchy order rebuild: %zu entity(ies) unreachable from any "
                    "root (parent cycle) — excluded from propagation. Run "
                    "repairHierarchyCycles() / fix the scene.\n",
                    Policy::kName, unresolved_count_);
#endif

            // Snapshot the packed (entity, parent) pairs in POOL order — the next
            // frames' change scan compares against exactly this.
            structure_cache_.clear();
            structure_cache_.reserve(n);
            for (std::size_t i = 0; i < n; ++i)
            {
                const auto e = st.data()[i];
                structure_cache_.emplace_back(entt::to_integral(e),
                                              entt::to_integral(st.get(e).parent));
            }
            order_valid_ = true;
        }

        static constexpr std::uint32_t kUnresolvedOrder = 0xFFFFFFFFu;
        /// Append batches above this rebuild instead: keeps bulk loads on the
        /// canonical (id-deterministic) order and bounds the fixpoint loop.
        static constexpr std::size_t kMaxAppendBatch = 256;

        /// Packed (entity, parent) integral pairs at the last order derivation — the
        /// per-frame structure scan compares against this (catches signal-less field
        /// writes; add/remove/reorder shows up as a size/prefix mismatch).
        std::vector<std::pair<std::uint32_t, std::uint32_t>> structure_cache_;
        /// The traversal order: resolved entities, every parent before its children.
        std::vector<lux::meta::entity_id> preorder_;
        /// entity index → rank in preorder_ (kUnresolvedOrder = not ordered). Only
        /// entries of CURRENT pool members are ever read.
        std::vector<std::uint32_t> order_;
        bool          order_valid_{false};
        std::size_t   unresolved_count_{0};

        // Rebuild / append scratch (capacity reused across frames).
        std::vector<std::pair<std::uint32_t, lux::meta::entity_id>> edges_;
        std::vector<lux::meta::entity_id> roots_;
        std::vector<lux::meta::entity_id> dfs_stack_;
        std::vector<lux::meta::entity_id> pending_appends_;
        /// Reused scratch for the ONE-TIME derived-maintenance backfill.
        std::vector<lux::meta::entity_id> scratch_;

        /// G-07 signals are wired lazily on the first update (see update()).
        bool derived_maintenance_connected_{false};
#ifndef NDEBUG
        const void* maintenance_registry_{nullptr};   // 1:1-registry tripwire
#endif
    };

} // namespace lux::gameplay
