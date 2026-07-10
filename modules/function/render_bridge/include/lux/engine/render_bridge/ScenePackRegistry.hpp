#pragma once
// ============================================================================
//  ScenePackRegistry.hpp — the pack self-registration table that replaces the
//  central install() god-function (ADR: lux-engine-pack-architecture §3,
//  lux::render_bridge).
//
//  A pack contributes ENTRIES: each entry backs a set of capability bits and
//  carries the callbacks that install its systems / register its bridges when
//  any of those bits is enabled. The two old invariants survive, strengthened:
//   - BACKING: `backedMask()` IS the registry content — an enabled capability
//     nothing registered for is refused by the caller (never a silently dead
//     capability). Promise and implementation are one struct literal.
//   - ORDER: entries run sorted by (order, insertion index). The canonical
//     system order is data, not the accident of one function's statement
//     sequence. Cross-entry prerequisites ("the fixed-step coordinator
//     exists") are order contracts checked by preflight() BEFORE anything
//     mutates the World — never-partial-install is preserved.
//
//  The plan travels as `const void*`: the registry is capability-mask generic;
//  a pack's callbacks cast to their own plan type (D2ScenePlan today). The
//  CALLER assembles the registry (add the packs it ships), which is exactly
//  how an external pack joins without touching any central file — the
//  physics2d demo pack is the first proof.
// ============================================================================

#include <lux/engine/render_bridge/SceneServices.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace lux::ecs { class World; }

namespace lux::render_bridge
{
    class RenderableSystem;

    /// Everything an installer/bridge callback may touch during assembly.
    /// `world` is null on the bridge-registration path (bridges must not
    /// touch it) — a pointer, not a reference, so that path stays well-formed.
    struct ScenePackContext
    {
        lux::ecs::World* world;
        SceneServices&   services;
        const void*      plan;   ///< pack-defined plan type; callbacks cast
    };

    struct ScenePackEntry
    {
        /// Capability bits this entry BACKS — the pack's PROMISE, and exactly
        /// what backedMask() reports. 0 = backs nothing (shared infrastructure
        /// or an unconditional entry).
        std::uint32_t backs{0};
        /// Bits that ACTIVATE the entry when enabled in the plan. 0 = derive
        /// from `backs`; when BOTH are 0 the entry is unconditional (the
        /// plan-less d3 shape). Kept separate from `backs` deliberately: a
        /// shared-prerequisite entry (the fixed-step coordinator) activates on
        /// several capabilities while PROMISING none of them.
        std::uint32_t active_on{0};
        /// Ascending execution stage (ties run in insertion order).
        int order{0};
        /// Pre-mutation check (optional): return false to refuse the WHOLE
        /// install before any entry ran (e.g. "my required service exists").
        bool (*preflight)(const ScenePackContext&){nullptr};
        /// Add systems / wire phases (optional).
        void (*install)(const ScenePackContext&){nullptr};
        /// Register renderable bridges (optional).
        void (*bridges)(RenderableSystem&, const ScenePackContext&){nullptr};
    };

    class ScenePackRegistry
    {
    public:
        void add(const ScenePackEntry& e) { entries_.push_back(e); }

        /// OR of every entry's `backs` bits — the live backing table.
        [[nodiscard]] std::uint32_t backedMask() const noexcept
        {
            std::uint32_t m = 0;
            for (const auto& e : entries_) m |= e.backs;
            return m;
        }

        /// Preflight + install every ACTIVE entry (activation mask 0, or any
        /// bit ∩ @p enabled), in (order, insertion) order. False = some
        /// preflight refused; the World was NOT touched.
        bool installAll(const ScenePackContext& ctx, std::uint32_t enabled) const
        {
            const auto active = sortedActive(enabled);
            for (const auto* e : active)
                if (e->preflight && !e->preflight(ctx))
                    return false;
            for (const auto* e : active)
                if (e->install)
                    e->install(ctx);
            return true;
        }

        /// Register every active entry's bridges, same ordering rules.
        void registerBridges(RenderableSystem& rs, const ScenePackContext& ctx,
                             std::uint32_t enabled) const
        {
            for (const auto* e : sortedActive(enabled))
                if (e->bridges)
                    e->bridges(rs, ctx);
        }

    private:
        [[nodiscard]] std::vector<const ScenePackEntry*> sortedActive(std::uint32_t enabled) const
        {
            std::vector<const ScenePackEntry*> v;
            v.reserve(entries_.size());
            for (const auto& e : entries_)
            {
                const std::uint32_t activation = e.active_on ? e.active_on : e.backs;
                if (activation == 0 || (activation & enabled) != 0)
                    v.push_back(&e);
            }
            std::stable_sort(v.begin(), v.end(),
                             [](const ScenePackEntry* a, const ScenePackEntry* b)
                             { return a->order < b->order; });
            return v;
        }

        std::vector<ScenePackEntry> entries_;
    };

} // namespace lux::render_bridge
