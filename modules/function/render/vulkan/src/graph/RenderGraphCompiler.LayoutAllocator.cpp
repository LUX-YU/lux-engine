// =============================================================================
//  RenderGraphCompiler.LayoutAllocator.cpp — graph-compile-time descriptor
//  layout allocation
// -----------------------------------------------------------------------------
//  computeGraphDescriptorLayouts: walks every pass -> pipeline -> slot in the
//  whole graph, classifies each slot, and aggregates the shapes of every set
//  used by this graph into a LayoutPlan.
//
//  THE AGGREGATION KEY IS OWNERSHIP, NOT SET NUMBER — this is the easiest
//  thing to get wrong in this file, and getting it wrong once already cost a
//  build where a real-world graph could not compile at all. A set number only
//  means something WITHIN A SINGLE PIPELINE:
//    - An engine-shared set's identity is its canonical number, but within a
//      given pipeline its slot may be overridden by a macro and moved
//      elsewhere (e.g. the skinning vertex pool lives at set 1, whose
//      canonical number is 7);
//    - Private sets with the same number in different pipelines have no
//      relationship to each other (Tonemap's set 1 is the HDR input, while
//      Canvas2D's set 1 is the instance buffer) — merging by number would
//      misclassify them as "conflicting declarations of the same set";
//    - A bindless unbounded array reflected from different shaders naturally
//      has a different count (e.g. uTex); requiring the count to match across
//      pipelines is equivalent to requiring they not be bindless at all.
//  So classification is recorded IN PLACE by PipelineManager while it builds
//  the layout (ReflectedSlot); this file only aggregates what was recorded,
//  it never re-derives it.
//
//  THE SHAPE OF AN ENGINE SET IS NOT TAKEN FROM REFLECTION — it comes from
//  EngineSetShapes. Reflection only contains the subset of bindings actually
//  used by this pipeline, while the set instance is allocated with the full
//  shape — describing it with the subset would be wrong (the Light set has
//  11 bindings; binding 0 of the Scene set isn't declared by any shader at
//  all).
//
//  The initial allocation strategy is identity: this file only does
//  "aggregate + produce a Plan" for now, and slots keep their current
//  assignment. That lets the whole chain be verified with zero behavior
//  change; changing the allocation strategy later only means replacing the
//  single assignSlots step.
//
//  Design: .internal/lux-engine-descriptor-layout-architecture.md §4.2
// =============================================================================
#include <lux/engine/render/graph/RenderGraphCompiler.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/description/LayoutContract.hpp>

#include <lux/cxx/algorithm/hash.hpp>

#include <algorithm>
#include <string>

namespace lux::render
{
namespace
{
    [[nodiscard]] const ReflectedSet* findReflectedSet(const PipelineReflectedInfo& refl,
                                                       uint32_t set) noexcept
    {
        for (const auto& rs : refl.reflected_sets)
        {
            if (rs.set == set)
            {
                return &rs;
            }
        }
        return nullptr;   // this slot is a reflection hole / a slot explicitly declared but not referenced by this pipeline's shader
    }

    /// Contract reverse-lookup: which logical resource lives at a given engine
    /// canonical location. Used only to attach a readable name to a binding in
    /// the Plan (the engine shape table itself carries no names — it describes
    /// what the engine side looks like, while the name is the reconciliation
    /// key on the shader side, and the two don't necessarily both exist).
    [[nodiscard]] const char* canonicalName(uint32_t set, uint32_t binding) noexcept
    {
        for (const auto& e : rdesc::kLayoutContractV0)
            if (e.engine_set && e.canonical_set == set && e.canonical_binding == binding)
            {
                return e.name;
            }
        return nullptr;
    }

    /// Fill one engine set from the engine shape table. Handling of count:
    /// device-derived bindless capacity is deliberately NOT resolved here —
    /// it's recorded as 0, meaning "runtime array, capacity determined by the
    /// engine" (matching count==0 in the contract). The benefit is that the
    /// Plan stays device-independent, so plan_hash is identical across
    /// machines.
    void fillFromEngineShape(PlannedSet& dst, uint32_t canonical_set)
    {
        if (canonical_set >= kEngineSetShapes.size())
        {
            return;
        }

        const EngineSetShape& shape = kEngineSetShapes[canonical_set];
        dst.update_after_bind = shape.update_after_bind;

        for (const auto& b : shape.bindings)
        {
            PlannedBinding pb{};
            pb.binding       = b.binding;
            pb.type          = b.type;
            pb.stages        = b.stages;
            pb.binding_flags = b.binding_flags;
            pb.count         = (b.count_source == EBindingCountSource::Fixed) ? b.count : 0u;
            if (const char* n = canonicalName(canonical_set, b.binding))
            {
                pb.name = n;
            }
            dst.bindings.push_back(std::move(pb));

            if (shape.expand_by_count_source)
            {
                break;   // template-style set (per-family Material): only record the template entry
            }
        }
    }

    /// Contract completion: binding flags and frequency domain (reflection
    /// can't recover UAB / PARTIALLY_BOUND / VARIABLE_COUNT). Stage is also
    /// taken from the contract — when the same logical resource is referenced
    /// by different stage subsets across pipelines, its set instance is
    /// allocated using the layout for the full stage union.
    void applyContract(PlannedSet& set)
    {
        for (auto& b : set.bindings)
        {
            const auto* e = rdesc::findLogicalResource(b.name);
            if (!e)
            {
                continue;
            }

            const auto f = e->binding_flags;
            if (f & static_cast<uint8_t>(rdesc::EBindingFlags::UPDATE_AFTER_BIND))
            {
                b.binding_flags |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
                set.update_after_bind = true;
            }
            if (f & static_cast<uint8_t>(rdesc::EBindingFlags::PARTIALLY_BOUND))
            {
                b.binding_flags |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
            }
            if (f & static_cast<uint8_t>(rdesc::EBindingFlags::VARIABLE_COUNT))
            {
                b.binding_flags |= VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
            }

            VkShaderStageFlags contract_stages = 0;
            if (e->stages & static_cast<uint8_t>(rdesc::EStageBits::VERTEX))
            {
                contract_stages |= VK_SHADER_STAGE_VERTEX_BIT;
            }
            if (e->stages & static_cast<uint8_t>(rdesc::EStageBits::FRAGMENT))
            {
                contract_stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            if (e->stages & static_cast<uint8_t>(rdesc::EStageBits::COMPUTE))
            {
                contract_stages |= VK_SHADER_STAGE_COMPUTE_BIT;
            }
            if (contract_stages != 0)
            {
                b.stages = contract_stages;
            }

            set.frequency = e->frequency;
        }
    }

    /// Which update-frequency domain a slot belongs to — the grouping key for
    /// the <=4-set projection.
    ///
    /// Each of the three ownership kinds has its own source: an engine set's
    /// domain is an engine-level fact (look up EngineSetShapes); a set a
    /// feature explicitly shares is by definition the FEATURE domain; a set
    /// private to a single pipeline is PASS_LOCAL. A reflection hole follows
    /// whichever engine set it's a placeholder for.
    [[nodiscard]] rdesc::EBindFrequency slotFrequency(const ReflectedSlot& slot) noexcept
    {
        switch (slot.source)
        {
        case ESlotSource::EngineShared:
        case ESlotSource::ReflectionHole:
            return slot.logical_set < kEngineSetShapes.size()
                 ? kEngineSetShapes[slot.logical_set].frequency
                 : rdesc::EBindFrequency::FEATURE;
        case ESlotSource::FeatureExplicit:
            return rdesc::EBindFrequency::FEATURE;
        case ESlotSource::PipelinePrivate:
            return rdesc::EBindFrequency::PASS_LOCAL;
        case ESlotSource::DomainMerged:
            // logical_set IS the domain enum value (see the ESlotSource comment).
            return slot.logical_set < 4u
                 ? static_cast<rdesc::EBindFrequency>(slot.logical_set)
                 : rdesc::EBindFrequency::FEATURE;
        }
        return rdesc::EBindFrequency::PASS_LOCAL;
    }

    /// Accumulate one binding's usage into StageUsage, bucketed by descriptor
    /// type. The three places that tally this (per-pipeline before merging,
    /// domain union, per-pipeline after merging) must use the same bucketing
    /// rule — the third place used to only accumulate the total without
    /// bucketing by type, which skewed the per-type readings that the §4.2
    /// feasibility conclusions depend on.
    void addDescriptorToUsage(LayoutPlan::StageUsage& u, VkDescriptorType type, uint32_t n) noexcept
    {
        switch (type)
        {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: u.uniform_buffer += n; break;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: u.storage_buffer += n; break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:          u.sampled_image  += n; break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:          u.storage_image  += n; break;
        default: break;
        }
        u.total += n;
    }

    /// Deterministic hash: independent of traversal order (the set is stably
    /// sorted first), so the same set of resources always yields the same
    /// value.
    [[nodiscard]] uint64_t hashPlan(const LayoutPlan& plan) noexcept
    {
        uint64_t h = 1469598103934665603ull;   // FNV-1a offset basis
        const auto mix = [&h](uint64_t v) noexcept {
            h ^= v;
            h *= 1099511628211ull;
        };
        for (const auto& s : plan.sets)
        {
            mix(s.set);
            mix(static_cast<uint64_t>(s.kind));
            mix(s.owner_key);
            for (const auto& b : s.bindings)
            {
                mix(b.binding);
                mix(static_cast<uint64_t>(b.type));
                mix(b.count);
                mix(static_cast<uint64_t>(b.stages));
                mix(static_cast<uint64_t>(b.binding_flags));
                mix(lux::cxx::algorithm::fnv1a(b.name));
            }
        }
        return h;
    }
} // namespace

bool RenderGraphCompiler::computeGraphDescriptorLayouts(RGCompiledGraph& compiled,
                                                        PipelineManager& pipeline_manager)
{
    LayoutPlan plan{};

    // -- 1. Whole-graph aggregation --------------------------------------
    const auto findOrAdd = [&plan](EPlannedSetKind kind, uint32_t set, uint64_t owner)
        -> PlannedSet&
    {
        for (auto& s : plan.sets)
            if (s.kind == kind && s.set == set && s.owner_key == owner)
            {
                return s;
            }
        PlannedSet ns{};
        ns.set       = set;
        ns.kind      = kind;
        ns.owner_key = owner;
        plan.sets.push_back(std::move(ns));
        return plan.sets.back();
    };

    /// Merge one pipeline's reflection at a given slot into the target set.
    /// A feature set shared across pipelines will land here multiple times:
    /// stages are unioned, count takes the upper bound (a bindless unbounded
    /// array reflects a different count in different shaders, and the larger
    /// one is the set's true capacity), and a type mismatch is recorded as a
    /// diagnostic rather than rejected — see the comment on LayoutPlan::warnings.
    const auto mergeReflectedInto = [&plan](PlannedSet& dst, const ReflectedSet& rs)
    {
        for (const auto& src : rs.bindings)
        {
            PlannedBinding* hit = nullptr;
            for (auto& b : dst.bindings)
                if (b.binding == src.binding) { hit = &b; break; }

            if (!hit)
            {
                PlannedBinding pb{};
                pb.binding = src.binding;
                pb.type    = src.type;
                pb.count   = src.count;
                pb.stages  = src.stages;
                pb.name    = src.name;
                dst.bindings.push_back(std::move(pb));
                continue;
            }

            hit->stages |= src.stages;
            hit->count   = std::max(hit->count, src.count);
            if (hit->type != src.type)
                plan.warnings.push_back(renderError<err::graph::SharedSetBindingTypeConflict>(
                    dst.set, src.binding));
        }
    };

    const auto absorb = [&](const PipelineReflectedInfo* refl, uint64_t owner)
    {
        // No reflection = a legacy-path pipeline (the caller supplies its own
        // pipeline_layout); its layout isn't managed here.
        if (!refl || refl->slots.empty())
        {
            return;
        }

        for (const auto& slot : refl->slots)
        {
            switch (slot.source)
            {
            case ESlotSource::EngineShared:
            case ESlotSource::ReflectionHole:
            {
                // A domain-mode hole is a MEANINGLESS placeholder (logical is
                // a sentinel value, see the hole comment in
                // buildReflectedPipelineLayout) — it doesn't go into the Plan,
                // otherwise it would generate an empty fake EngineShared entry
                // that pollutes both aggregation and plan_hash.
                if (slot.logical_set >= kEngineSetShapes.size())
                {
                    break;
                }
                // Identity is the canonical number; the shape comes from the
                // engine table, not from reflection (see the file header).
                PlannedSet& dst = findOrAdd(EPlannedSetKind::EngineShared,
                                            slot.logical_set, 0);
                if (dst.bindings.empty())
                {
                    fillFromEngineShape(dst, slot.logical_set);
                }
                // layout is simply the engine-shared layout for this canonical
                // slot — it was already resolved at registration time
                // (including device-derived bindless capacity), so it's reused
                // as-is here rather than rebuilt.
                dst.layout = slot.layout;
                break;
            }
            case ESlotSource::FeatureExplicit:
            {
                // Identity is the layout handle: multiple pipelines that share
                // the same feature set pass in the same handle, so they merge
                // naturally on that basis; two different features that happen
                // to both use set 3 will never be conflated.
                PlannedSet& dst = findOrAdd(EPlannedSetKind::FeatureExplicit, slot.slot,
                                            reinterpret_cast<uint64_t>(slot.layout));
                dst.layout = slot.layout;
                if (const auto* rs = findReflectedSet(*refl, slot.slot))
                {
                    mergeReflectedInto(dst, *rs);
                }
                break;
            }
            case ESlotSource::PipelinePrivate:
            {
                // Attributed to the specific pipeline — private sets with the
                // same number are NEVER merged across pipelines.
                PlannedSet& dst = findOrAdd(EPlannedSetKind::PipelinePrivate,
                                            slot.slot, owner);
                dst.layout = slot.layout;
                if (const auto* rs = findReflectedSet(*refl, slot.slot))
                {
                    mergeReflectedInto(dst, *rs);
                }
                break;
            }
            case ESlotSource::DomainMerged:
            {
                // Merged domain slot: one slot houses multiple engine sets.
                // Aggregate them INDIVIDUALLY, by member canonical, into their
                // own EngineShared PlannedSet — the Plan operates at the
                // "engine set" granularity, and merges into the same entries
                // used by non-switched pipelines, so under identity mapping
                // the two sides must agree. Member identity is looked up from
                // the reflected binding's name via the contract.
                //
                // The member's layout field is left untouched: that's the
                // per-set shared layout (still used by pipelines that haven't
                // switched over); the merged pipeline's own slot layout (the
                // domain layout) is taken directly from slot.layout in the
                // DomainMerged branch of layoutForSlot, bypassing the Plan.
                const auto* rs = findReflectedSet(*refl, slot.slot);
                if (!rs)
                {
                    break;
                }
                for (const auto& b : rs->bindings)
                {
                    const auto* e = engineOwnedResource(b.name);
                    if (!e)
                    {
                        continue;   // domain-mode registration already guarantees this can't happen; defensive only
                    }
                    PlannedSet& dst = findOrAdd(EPlannedSetKind::EngineShared,
                                                e->canonical_set, 0);
                    if (dst.bindings.empty())
                    {
                        fillFromEngineShape(dst, e->canonical_set);
                    }
                }
                break;
            }
            }
        }
    };

    // All pipelines within the same pass MUST have mutually compatible
    // layouts: descriptor sets are bound once per pass at record time (using
    // the primary pipeline's layout), while drawing switches between the
    // various geometry/family variant pipelines. If a variant's own layout is
    // incompatible with the layout used at bind time, that's VUID-...-08600.
    //
    // Before the switch to reflection-driven per-pipeline layouts this
    // invariant was free — every pipeline used the same fixed eight-set
    // standard layout. Once layouts started being built per-pipeline from
    // reflection, nothing guarantees it any more, and a violation ONLY SHOWS
    // UP AS A RUNTIME ERROR. Catch it here at compile time instead, and point
    // out exactly which slot disagrees.
    const auto checkVariantCoherence = [&](const RGPassDescription& pass, std::uint32_t pass_index)
    {
        if (pass.additional_pipelines.empty() || !pass.pipeline_template.valid())
        {
            return;
        }
        const auto* base = pipeline_manager.templateReflection(pass.pipeline_template);
        if (!base || base->slots.empty())
        {
            return;
        }

        for (const auto& extra : pass.additional_pipelines)
        {
            if (!extra.valid())
            {
                continue;
            }
            const auto* var = pipeline_manager.templateReflection(extra);
            if (!var || var->slots.empty())
            {
                continue;
            }

            const std::size_t common = std::min(base->slots.size(), var->slots.size());
            for (std::size_t i = 0; i < common; ++i)
            {
                if (base->slots[i].layout == var->slots[i].layout)
                {
                    continue;
                }
                plan.warnings.push_back(renderError<err::graph::VariantLayoutIncompatible>(
                    pass_index, static_cast<std::uint32_t>(i)));
                break;   // one diagnostic per pass is enough; later slots would necessarily be incompatible too
            }
            if (base->slots.size() != var->slots.size())
            {
                plan.warnings.push_back(renderError<err::graph::VariantSetCountMismatch>(
                    pass_index,
                    static_cast<std::uint32_t>(base->slots.size()),
                    static_cast<std::uint32_t>(var->slots.size())));
            }
        }
    };

    for (std::uint32_t pass_index = 0;
         pass_index < compiled.original_graph.passes.size(); ++pass_index)
    {
        const auto& pass = compiled.original_graph.passes[pass_index];

        if (pass.pipeline_template.valid())
        {
            absorb(pipeline_manager.templateReflection(pass.pipeline_template),
                   graphicsOwnerKey(pass.pipeline_template.index));
        }

        for (const auto& extra : pass.additional_pipelines)
        {
            if (extra.valid())
            {
                absorb(pipeline_manager.templateReflection(extra),
                       graphicsOwnerKey(extra.index));
            }
        }

        if (pass.compute_pipeline_handle.valid())
        {
            absorb(pipeline_manager.computeReflection(pass.compute_pipeline_handle),
                   computeOwnerKey(pass.compute_pipeline_handle.index));
        }

        checkVariantCoherence(pass, pass_index);
    }

    // -- 2. Contract completion + stable sort (hash independent of traversal order) --
    for (auto& s : plan.sets)
    {
        applyContract(s);
        std::sort(s.bindings.begin(), s.bindings.end(),
                  [](const PlannedBinding& a, const PlannedBinding& b) {
                      return a.binding < b.binding;
                  });
    }
    std::sort(plan.sets.begin(), plan.sets.end(),
              [](const PlannedSet& a, const PlannedSet& b) {
                  if (a.kind != b.kind)
                  {
                      return a.kind < b.kind;
                  }
                  if (a.set != b.set)
                  {
                      return a.set < b.set;
                  }
                  return a.owner_key < b.owner_key;
              });

    // -- 3. Slot assignment -----------------------------------------------
    // Initial approach: identity (keep the current slot assignment), so the
    // whole chain can be verified with zero behavior change. Changing the
    // allocation strategy only requires replacing this step.
    //
    // 3b. Domain partitioning and intra-domain binding offsets (computed but
    //     not used yet, produced alongside the identity assignment). After
    //     merging, multiple engine sets in the same domain live inside a
    //     single set, and each of them currently numbers its bindings
    //     starting at 0 — they must be shifted into non-overlapping ranges.
    //
    //     Assign in ascending canonical-set-number order to keep this
    //     deterministic: the same set of features always gets the same
    //     offsets, independent of traversal order or the order of passes in
    //     the graph.
    {
        // The offset is an ENGINE-LEVEL CONSTANT computed from the full shape
        // table (engineSetDomainOffset) — it is NOT accumulated from "which
        // sets this graph happens to use". A domain set is a single
        // scene-level instance, so computing it per-graph would place
        // Material at offset 2 instead of 13 in a graph that has no Light,
        // making the domain layouts of the two graphs incompatible. This step
        // just records the constant into the Plan for downstream readers.
        for (auto& s : plan.sets)
        {
            if (s.kind != EPlannedSetKind::EngineShared || s.set >= kEngineSetShapes.size())
            {
                continue;
            }
            s.domain                = kEngineSetShapes[s.set].frequency;
            s.domain_binding_offset = engineSetDomainOffset(s.set);
            s.domain_binding_count  = engineSetOccupancy(s.set);
        }

        // Feature-owned / private sets don't participate in domain merging
        // (the conservative accounting has proven sufficient in practice);
        // they're tagged with their own domain but the offset is always 0 —
        // they already have an entire set to themselves.
        for (auto& s : plan.sets)
        {
            if (s.kind == EPlannedSetKind::EngineShared)
            {
                continue;
            }
            s.domain = (s.kind == EPlannedSetKind::FeatureExplicit)
                     ? rdesc::EBindFrequency::FEATURE
                     : rdesc::EBindFrequency::PASS_LOCAL;
            s.domain_binding_offset = 0;
            s.domain_binding_count  = static_cast<uint32_t>(s.bindings.size());
        }

        // Self-check: ranges within the same domain must not overlap. An
        // overlap means a silent descriptor misalignment (writing into
        // someone else's binding) that Vulkan will never flag as an error, so
        // it has to be caught here.
        for (std::size_t i = 0; i < plan.sets.size(); ++i)
        {
            const auto& a = plan.sets[i];
            if (a.kind != EPlannedSetKind::EngineShared)
            {
                continue;
            }
            for (std::size_t j = i + 1; j < plan.sets.size(); ++j)
            {
                const auto& b = plan.sets[j];
                if (b.kind != EPlannedSetKind::EngineShared || b.domain != a.domain)
                {
                    continue;
                }
                const bool overlap =
                    a.domain_binding_offset < b.domain_binding_offset + b.domain_binding_count &&
                    b.domain_binding_offset < a.domain_binding_offset + a.domain_binding_count;
                if (overlap)
                    plan.warnings.push_back(renderError<err::graph::DomainBindingRangeOverlap>(
                        a.set, b.set, std::max(a.domain_binding_offset, b.domain_binding_offset)));
            }
        }
    }

    plan.plan_hash = hashPlan(plan);

    // -- 3c. Per-pipeline slot table (pure diagnostics, not part of plan_hash) --
    //  This is the observability tool for the migration effort: a gate
    //  rejection can silently fall back to the legacy path while the gate
    //  itself still reads green — so record, per pipeline, whether it
    //  switched over and what each slot is wired to, and print it via the
    //  graph dump (see the [Pipeline Slots] section in RGDebugPrint). Only
    //  then does "green" actually mean "switched over".
    {
        lux::cxx::SmallVector<uint32_t, 32> seen_gfx;
        lux::cxx::SmallVector<uint32_t, 16> seen_comp;

        const auto slotKindChar = [](ESlotSource s) -> char {
            switch (s)
            {
            case ESlotSource::EngineShared:    return 'E';
            case ESlotSource::FeatureExplicit: return 'X';
            case ESlotSource::PipelinePrivate: return 'P';
            case ESlotSource::ReflectionHole:  return 'H';
            case ESlotSource::DomainMerged:    return 'D';
            }
            return '?';
        };

        const auto record = [&](const PipelineReflectedInfo* refl, std::string name)
        {
            LayoutPlan::PipelineDiag diag{};
            diag.name = std::move(name);
            if (!refl || refl->slots.empty())
            {
                diag.legacy = true;
                plan.pipelines.push_back(std::move(diag));
                return;
            }
            diag.merged = refl->merged_domain_layout;
            // <=4-set assertion (the acceptance gate for the real allocation
            // strategy): warn whenever a merged pipeline's actual slot count
            // exceeds 4 — the gate treats a Plan warning as FAIL, and the
            // offending pipeline is named explicitly.
            if (diag.merged && refl->slots.size() > 4)
                plan.warnings.push_back(renderError<err::graph::MergedLayoutExceedsSetBudget>(
                    static_cast<std::uint32_t>(refl->slots.size())));
            // Identity check: none of the engine resources in the reflection
            // has a "merged position != current position" mismatch.
            if (!diag.merged)
            {
                bool movable = false;
                for (const auto& rs : refl->reflected_sets)
                    for (const auto& b : rs.bindings)
                        if (const auto* e = engineOwnedResource(b.name))
                        {
                            const uint32_t to_set =
                                domainSetSlot(kEngineSetShapes[e->canonical_set].frequency);
                            const uint32_t to_binding =
                                engineSetDomainOffset(e->canonical_set) + e->canonical_binding;
                            if (to_set != rs.set || to_binding != b.binding)
                            { movable = true; break; }
                        }
                diag.identity = !movable;
            }
            for (const auto& slot : refl->slots)
            {
                LayoutPlan::PipelineSlotDiag sd{};
                sd.slot    = slot.slot;
                sd.kind    = slotKindChar(slot.source);
                sd.logical = slot.logical_set;
                if (slot.source == ESlotSource::DomainMerged)
                    if (const auto* rs = findReflectedSet(*refl, slot.slot))
                        for (const auto& b : rs->bindings)
                            if (const auto* e = engineOwnedResource(b.name))
                            {
                                bool dup = false;
                                for (auto m : sd.members)
                                {
                                    if (m == e->canonical_set)
                                    {
                                        dup = true;
                                        break;
                                    }
                                }
                                if (!dup)
                                {
                                    sd.members.push_back(e->canonical_set);
                                }
                            }
                diag.slots.push_back(std::move(sd));
            }
            plan.pipelines.push_back(std::move(diag));
        };

        const auto recordGraphics = [&](GraphicsPipelineHandle handle)
        {
            for (auto i : seen_gfx)
            {
                if (i == handle.index)
                {
                    return;
                }
            }
            seen_gfx.push_back(handle.index);
            const auto& t = pipeline_manager.getTemplate(handle);
            record(pipeline_manager.templateReflection(handle),
                   t.debug_name.empty() ? ("template#" + std::to_string(handle.index))
                                        : t.debug_name);
        };

        for (const auto& pass : compiled.original_graph.passes)
        {
            if (pass.pipeline_template.valid())
            {
                recordGraphics(pass.pipeline_template);
            }
            for (const auto& extra : pass.additional_pipelines)
            {
                if (extra.valid())
                {
                    recordGraphics(extra);
                }
            }
            if (pass.compute_pipeline_handle.valid())
            {
                bool seen = false;
                for (auto i : seen_comp)
                {
                    if (i == pass.compute_pipeline_handle.index)
                    {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                {
                    seen_comp.push_back(pass.compute_pipeline_handle.index);
                    record(pipeline_manager.computeReflection(pass.compute_pipeline_handle),
                           "compute:" + pass.name);
                }
            }
        }
    }

    // -- 4. Plan-driven write-back -----------------------------------------
    // Make the Plan actually determine pipeline layouts: for each slot, look
    // it back up in the Plan using the SAME identity rule used during
    // aggregation, and rebuild the template's set array from the Plan's
    // layout. Under identity allocation, the result matches the original
    // array slot-for-slot, so rebuildTemplateLayout detects no change, leaves
    // the epoch untouched, and there's zero behavior change; once a real
    // allocation strategy replaces this, the very same code automatically
    // makes every template follow the Plan.
    //
    // Only graphics templates are written back: a compute pipeline's layout
    // is fixed once and for all by registerComputePipelineReflected, and it
    // never participates in in-pass variant switching, so there's no need for
    // it to follow the Plan.
    const auto layoutForSlot = [&plan](const ReflectedSlot& slot,
                                       uint64_t owner) -> VkDescriptorSetLayout
    {
        const PlannedSet* ps = nullptr;
        switch (slot.source)
        {
        case ESlotSource::EngineShared:
        case ESlotSource::ReflectionHole:
            // A domain-mode semantics-free placeholder hole isn't managed by
            // the Plan; keep the layout from registration time — otherwise
            // failing to find it would make the whole template give up on
            // write-back.
            if (slot.logical_set >= kEngineSetShapes.size())
            {
                return slot.layout;
            }
            ps = plan.find(slot.logical_set, EPlannedSetKind::EngineShared, 0);
            break;
        case ESlotSource::FeatureExplicit:
            ps = plan.find(slot.slot, EPlannedSetKind::FeatureExplicit,
                           reinterpret_cast<uint64_t>(slot.layout));
            break;
        case ESlotSource::PipelinePrivate:
            ps = plan.find(slot.slot, EPlannedSetKind::PipelinePrivate, owner);
            break;
        case ESlotSource::DomainMerged:
            // The domain layout is an engine-level constant (expanded from
            // the shape table); the Plan has no room to reassign it, so just
            // reuse the domain layout fixed at registration time — it doesn't
            // consult the Plan, so it's unaffected by whether member entries
            // carry a layout.
            return slot.layout;
        }
        return ps ? ps->layout : VK_NULL_HANDLE;
    };

    const auto writeBack = [&](GraphicsPipelineHandle handle)
    {
        const auto* refl = pipeline_manager.templateReflection(handle);
        if (!refl || refl->slots.empty())
        {
            return;   // legacy-path pipeline (caller supplies its own layout), not managed by the Plan
        }

        lux::cxx::SmallVector<VkDescriptorSetLayout, 8> layouts;
        const uint64_t owner = graphicsOwnerKey(handle.index);
        for (const auto& slot : refl->slots)
        {
            VkDescriptorSetLayout l = layoutForSlot(slot, owner);
            // The Plan was aggregated from these very slots, so a failed
            // lookup means the two sides come from different sources — don't
            // guess, leave the template as-is (the return on the next line
            // abandons write-back for this template).
            if (l == VK_NULL_HANDLE)
            {
                return;
            }
            layouts.push_back(l);
        }
        auto changed = pipeline_manager.rebuildTemplateLayout(
            handle, {layouts.data(), layouts.size()});
        if (changed && *changed)
            ++plan.templates_relaid;

        // Cross-check that resource_slot_map is derivable (check-only, this
        // never modifies anything — see the note in LayoutPlan). Derivation
        // rule: engine sets and reflection-hole slots map to {canonical
        // semantics, this pipeline's slot number}. Private / feature-explicit
        // sets don't enter this table — they carry no engine-resource
        // semantics, and the feature binds them itself at record time.
        lux::cxx::SmallVector<std::pair<EDescriptorSetSlot, uint32_t>, 8> derived;
        for (const auto& slot : refl->slots)
        {
            // Expand a merged domain slot's members (matching the same
            // synthesis rule used for resource_slot_map at registration time
            // — otherwise the cross-check would always warn).
            if (slot.source == ESlotSource::DomainMerged)
            {
                const auto* rs = findReflectedSet(*refl, slot.slot);
                if (!rs)
                {
                    continue;
                }
                for (const auto& b : rs->bindings)
                {
                    const auto* e = engineOwnedResource(b.name);
                    if (!e || e->canonical_set >= kDescriptorSetCount)
                    {
                        continue;
                    }
                    const auto logical = static_cast<EDescriptorSetSlot>(e->canonical_set);
                    bool seen = false;
                    for (const auto& [ls, li] : derived)
                    {
                        if (ls == logical && li == slot.slot)
                        {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen)
                    {
                        derived.push_back({logical, slot.slot});
                    }
                }
                continue;
            }
            if (slot.source != ESlotSource::EngineShared &&
                slot.source != ESlotSource::ReflectionHole)
                continue;
            if (slot.logical_set >= kDescriptorSetCount)
            {
                continue;
            }
            derived.push_back({static_cast<EDescriptorSetSlot>(slot.logical_set), slot.slot});
        }

        const auto& gtmpl    = pipeline_manager.getTemplate(handle);
        const auto& authored = gtmpl.resource_slot_map;
        const std::string who = gtmpl.debug_name.empty()
                              ? ("template#" + std::to_string(handle.index))
                              : gtmpl.debug_name;
        const auto contains = [](const auto& list, EDescriptorSetSlot s, uint32_t idx) {
            for (const auto& [ls, li] : list)
            {
                if (ls == s && li == idx)
                {
                    return true;
                }
            }
            return false;
        };
        // For domain-mode pipelines, translate the hand-authored semantics
        // through their corresponding domain slot before checking: a
        // hand-authored {Scene -> 0} correctly means "the GLOBAL domain slot"
        // in domain mode — the hole is a semantics-free placeholder, so it
        // can't be derived, but as long as the authored slot number equals
        // the domain slot number for that semantic's domain, the behavior is
        // correct (the bare-slot bindSceneDS binds the domain instance with
        // the domain layout, which is compatible). Only warn when the
        // hand-authored slot points at the wrong domain slot.
        const bool merged_pipeline = refl->merged_domain_layout;
        const auto authoredOkInMergedMode = [&](EDescriptorSetSlot s, uint32_t idx) {
            const auto canonical = static_cast<uint32_t>(s);
            if (canonical >= kEngineSetShapes.size())
            {
                return false;
            }
            return idx == domainSetSlot(kEngineSetShapes[canonical].frequency);
        };
        for (const auto& [s, idx] : authored)
        {
            if (!contains(derived, s, idx) &&
                !(merged_pipeline && authoredOkInMergedMode(s, idx)))
                plan.warnings.push_back(renderError<err::graph::AuthoredDerivedSlotMismatch>(
                    static_cast<std::uint32_t>(s), idx, 0u));
        }
        for (const auto& [s, idx] : derived)
        {
            if (!contains(authored, s, idx))
                plan.warnings.push_back(renderError<err::graph::AuthoredDerivedSlotMismatch>(
                    static_cast<std::uint32_t>(s), idx, 1u));
        }
    };

    for (const auto& pass : compiled.original_graph.passes)
    {
        if (pass.pipeline_template.valid())
        {
            writeBack(pass.pipeline_template);
        }
        for (const auto& extra : pass.additional_pipelines)
        {
            if (extra.valid())
            {
                writeBack(extra);
            }
        }
    }

    // -- 5. <=4-set feasibility projection (computed but not used yet, see the note in LayoutPlan) --
    // This measures the WIDEST single pipeline — maxBoundDescriptorSets
    // constrains how many sets a single pipeline binds, not how many the
    // whole graph uses in total.
    //
    // The budget is Vulkan's REQUIRED MINIMUM, not this device's limit —
    // the same reasoning as stage_over_min further down, and for the same
    // reason it must not be the local number: maxBoundDescriptorSets is 32
    // on a desktop NVIDIA part and 4 on Mali, so measuring against the local
    // device makes this projection structurally unable to fire during
    // development. It would take 33 sets to trip here while the phone
    // already rejects 5 — i.e. exactly the signal we built it for would be
    // the one it can never produce.
    //
    // The device's own limit is not ignored, it is just enforced elsewhere
    // and at a different time: PipelineLayoutService::getOrCreate fails the
    // layout outright when it exceeds the running device. That is the
    // "will this run HERE" check. This one answers "will this run on the
    // MINIMUM CONFORMANT device", which is the only question a desktop run
    // can answer about a phone.
    constexpr uint32_t kMaxBoundDescriptorSetsRequiredMin = 4;  // Vulkan "Required Limits"
    const uint32_t budget = kMaxBoundDescriptorSetsRequiredMin;
    /// Which pass owns the widest projection under conservative accounting.
    /// Carried out of the loop so the per-graph warning below can name a pass
    /// instead of just a number.
    uint32_t worst_strict_pass = 0;
    const auto project = [&](const PipelineReflectedInfo* refl,
                             const char*                  name,
                             uint32_t                     pass_index)
    {
        if (!refl || refl->slots.empty())
        {
            return;
        }

        plan.max_sets_current =
            std::max(plan.max_sets_current, static_cast<uint32_t>(refl->slots.size()));

        // Domain dedup: multiple sets in the same domain merge into one.
        // Optimistic accounting: feature-owned shared sets are also folded
        //           into the FEATURE domain.
        // Conservative accounting: feature-owned shared sets each count
        //           separately (deduped by layout handle — pipelines sharing
        //           the same one still count as one).
        bool domain_used[4] = {false, false, false, false};
        lux::cxx::SmallVector<VkDescriptorSetLayout, 4> explicit_sets;
        for (const auto& slot : refl->slots)
        {
            domain_used[static_cast<std::size_t>(slotFrequency(slot))] = true;
            if (slot.source == ESlotSource::FeatureExplicit)
            {
                bool seen = false;
                for (auto l : explicit_sets)
                {
                    if (l == slot.layout)
                    {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                {
                    explicit_sets.push_back(slot.layout);
                }
            }
        }

        uint32_t projected = 0;
        for (bool used : domain_used)
        {
            projected += used ? 1u : 0u;
        }

        // Conservative accounting: pull feature-owned sets out of the FEATURE
        // domain and count them separately. If this pipeline's FEATURE domain
        // consists ONLY of feature-owned sets, the domain is empty once
        // they're pulled out, so it must not be double-counted — hence first
        // check whether any engine set falls in the FEATURE domain.
        bool feature_domain_has_engine = false;
        for (const auto& slot : refl->slots)
            if (slot.source != ESlotSource::FeatureExplicit &&
                slotFrequency(slot) == rdesc::EBindFrequency::FEATURE)
            { feature_domain_has_engine = true; break; }

        uint32_t strict = projected;
        if (!explicit_sets.empty())
        {
            if (!feature_domain_has_engine) --strict;      // the FEATURE domain was already empty
            strict += static_cast<uint32_t>(explicit_sets.size());
        }

        plan.max_sets_projected = std::max(plan.max_sets_projected, projected);
        if (strict > plan.max_sets_projected_strict)
        {
            plan.max_sets_projected_strict = strict;
            worst_strict_pass              = pass_index;
        }
        // No `budget > 0` guard any more: the budget is a spec constant, not a
        // queried value that could come back as "unknown".
        if (projected > budget)
            plan.over_budget.push_back(std::string{name} + "(" +
                                       std::to_string(projected) + " > " +
                                       std::to_string(budget) + ")");
        if (strict > budget)
            plan.over_budget_strict.push_back(std::string{name} + "(" +
                                              std::to_string(strict) + " > " +
                                              std::to_string(budget) + ")");
    };

    // -- 6. Per-stage descriptor usage --------------------------------------
    // Another hard constraint that is UNRELATED to the set count: the
    // per-stage limit accumulates across ALL bound sets, so merging sets
    // doesn't reduce the descriptor count by even one. Only non-UAB bindings
    // are counted here (UAB bindings fall under the separate, much larger
    // maxPerStageDescriptorUpdateAfterBind* limits).
    //
    // Tally PER PIPELINE, then take the max across stages — the per-stage
    // limit constrains "how many descriptors a single stage of a single
    // pipeline can see," not the sum across the whole graph. Accumulating
    // over the whole graph would produce a wildly inflated number (a graph
    // with a dozen-plus compute passes could rack up dozens of SSBOs, when no
    // single shader ever uses anywhere near that many).
    LayoutPlan::StageUsage vertex{}, fragment{}, compute{};

    const auto accumulate = [&](const PipelineReflectedInfo* refl, uint64_t owner,
                                const char* pass_name)
    {
        if (!refl || refl->slots.empty())
        {
            return;
        }

        LayoutPlan::StageUsage v{}, f{}, c{};
        // Under the pre-merge accounting, a merged domain slot is equivalent
        // to binding each of its member sets — expand the members and
        // accumulate them individually, matching the same accounting used
        // for pipelines that haven't switched over.
        lux::cxx::SmallVector<const PlannedSet*, 8> slot_sets;
        for (const auto& slot : refl->slots)
        {
            slot_sets.clear();
            switch (slot.source)
            {
            case ESlotSource::EngineShared:
            case ESlotSource::ReflectionHole:
                if (const auto* ps = plan.find(slot.logical_set, EPlannedSetKind::EngineShared, 0))
                {
                    slot_sets.push_back(ps);
                }
                break;
            case ESlotSource::FeatureExplicit:
                if (const auto* ps = plan.find(slot.slot, EPlannedSetKind::FeatureExplicit,
                                               reinterpret_cast<uint64_t>(slot.layout)))
                    slot_sets.push_back(ps);
                break;
            case ESlotSource::PipelinePrivate:
                if (const auto* ps = plan.find(slot.slot, EPlannedSetKind::PipelinePrivate, owner))
                {
                    slot_sets.push_back(ps);
                }
                break;
            case ESlotSource::DomainMerged:
                if (const auto* rs = findReflectedSet(*refl, slot.slot))
                    for (const auto& b : rs->bindings)
                        if (const auto* e = engineOwnedResource(b.name))
                            if (const auto* ps = plan.find(e->canonical_set,
                                                           EPlannedSetKind::EngineShared, 0))
                            {
                                bool seen = false;
                                for (const auto* p : slot_sets) if (p == ps) { seen = true; break; }
                                if (!seen)
                                {
                                    slot_sets.push_back(ps);
                                }
                            }
                break;
            }

            for (const auto* ps : slot_sets)
            for (const auto& b : ps->bindings)
            {
                if (b.binding_flags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT)
                {
                    continue;   // falls under the UAB limit, doesn't count against this budget
                }
                // count==0 means "runtime array, capacity determined by the
                // engine" — this kind is necessarily UAB and has already been
                // filtered out above; any remaining 0 is treated as 1 so
                // nothing is missed.
                const uint32_t n = b.count == 0 ? 1u : b.count;

                if (b.stages & VK_SHADER_STAGE_VERTEX_BIT)
                {
                    addDescriptorToUsage(v, b.type, n);
                }
                if (b.stages & VK_SHADER_STAGE_FRAGMENT_BIT)
                {
                    addDescriptorToUsage(f, b.type, n);
                }
                if (b.stages & VK_SHADER_STAGE_COMPUTE_BIT)
                {
                    addDescriptorToUsage(c, b.type, n);
                }
            }
        }

        // Take the max per field — the peak for each type may come from a
        // different pipeline, so taking the max independently for each is
        // the correct upper bound for "no single pipeline exceeds this".
        const auto keepMax = [](LayoutPlan::StageUsage& dst, const LayoutPlan::StageUsage& s) {
            dst.uniform_buffer = std::max(dst.uniform_buffer, s.uniform_buffer);
            dst.storage_buffer = std::max(dst.storage_buffer, s.storage_buffer);
            dst.sampled_image  = std::max(dst.sampled_image,  s.sampled_image);
            dst.storage_image  = std::max(dst.storage_image,  s.storage_image);
            dst.total          = std::max(dst.total,          s.total);
        };
        keepMax(vertex, v);
        keepMax(fragment, f);
        keepMax(compute, c);

        // Violations are flagged PER PIPELINE, by name — reporting only a
        // single whole-graph maximum would just tell the reader "something
        // somewhere is over budget, go find it yourself". The comparison is
        // against the VULKAN MINIMUM GUARANTEE, not this machine's actual
        // limit: this machine is a discrete desktop GPU with much more
        // headroom, but that doesn't mean the target mobile device has the
        // same headroom. The numbers come from the Vulkan spec's
        // "Required Limits" table.
        const auto checkStage = [&](const char* stage, const LayoutPlan::StageUsage& u) {
            const auto flag = [&](uint32_t got, uint32_t min, const char* what) {
                if (got > min)
                    plan.stage_over_min.push_back(
                        std::string{pass_name} + " / " + stage + " " + what + " " +
                        std::to_string(got) + " > Vulkan 最低保证 " + std::to_string(min));
            };
            flag(u.uniform_buffer, 12u,  "uniform buffer");
            flag(u.storage_buffer, 4u,   "storage buffer");
            flag(u.sampled_image,  16u,  "sampled image");
            flag(u.storage_image,  4u,   "storage image");
            flag(u.total,          128u, "resources");
        };
        checkStage("VERTEX",   v);
        checkStage("FRAGMENT", f);
        checkStage("COMPUTE",  c);
    };

    // Index-based: EErrorArg::GraphPass carries a passes[] subscript, which is
    // how this error vocabulary names a pass (its name is user-authored and
    // doesn't fit a 32-bit arg slot — the client resolves it via
    // DumpRenderGraph). So the projection needs the index, not just the name.
    for (std::size_t pi = 0; pi < compiled.original_graph.passes.size(); ++pi)
    {
        const auto&    pass       = compiled.original_graph.passes[pi];
        const uint32_t pass_index = static_cast<uint32_t>(pi);
        if (pass.pipeline_template.valid())
        {
            project(pipeline_manager.templateReflection(pass.pipeline_template),
                    pass.name.c_str(), pass_index);
            accumulate(pipeline_manager.templateReflection(pass.pipeline_template),
                       graphicsOwnerKey(pass.pipeline_template.index), pass.name.c_str());
        }
        for (const auto& extra : pass.additional_pipelines)
            if (extra.valid())
            {
                project(pipeline_manager.templateReflection(extra), pass.name.c_str(), pass_index);
                accumulate(pipeline_manager.templateReflection(extra),
                           graphicsOwnerKey(extra.index), pass.name.c_str());
            }
        if (pass.compute_pipeline_handle.valid())
        {
            project(pipeline_manager.computeReflection(pass.compute_pipeline_handle),
                    pass.name.c_str(), pass_index);
            accumulate(pipeline_manager.computeReflection(pass.compute_pipeline_handle),
                       computeOwnerKey(pass.compute_pipeline_handle.index), pass.name.c_str());
        }
    }

    // One warning per GRAPH, not per pipeline. The finding — "domain merging
    // still leaves the widest pipeline over the portable budget" — is a
    // property of the graph, and the per-pipeline list already exists in
    // over_budget_strict for the dump. Emitting one per offender would put a
    // dozen near-identical lines into the log on every scene change for as
    // long as the convergence work is in flight, and a signal that noisy
    // stops being read — which is the same failure as hiding it behind a
    // debug switch, just louder.
    if (!plan.over_budget_strict.empty())
        plan.warnings.push_back(renderError<err::graph::ProjectedLayoutExceedsSetBudget>(
            worst_strict_pass, plan.max_sets_projected_strict));

    // -- 6b. Post-merge per-stage usage -------------------------------------
    // Merging sets isn't free: once multiple sets in the same domain are
    // merged into one, EVERY pipeline that binds it sees ALL of its bindings.
    // So the price of a lower set count is a higher per-stage descriptor
    // count, and that has to be measured separately — using the pre-merge
    // numbers to argue that the merge is safe would be wrong.
    //
    // Algorithm: first compute the union usage of each domain's set
    // (whole-graph accounting, since a merged domain set is one scene-level
    // instance), then, for each pipeline, accumulate the domains it actually
    // binds.
    LayoutPlan::StageUsage domain_v[4]{}, domain_f[4]{}, domain_c[4]{};
    for (const auto& s : plan.sets)
    {
        // Only engine sets and feature-shared sets get folded into a domain
        // set; a private set already belongs to just one pipeline, so it
        // only ever has its own share, before or after merging.
        std::size_t domain;
        if (s.kind == EPlannedSetKind::EngineShared)
            domain = static_cast<std::size_t>(
                s.set < kEngineSetShapes.size()
                    ? kEngineSetShapes[s.set].frequency : rdesc::EBindFrequency::FEATURE);
        else if (s.kind == EPlannedSetKind::FeatureExplicit)
        {
            domain = static_cast<std::size_t>(rdesc::EBindFrequency::FEATURE);
        }
        else
            continue;

        for (const auto& b : s.bindings)
        {
            if (b.binding_flags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT)
            {
                continue;
            }
            const uint32_t n = b.count == 0 ? 1u : b.count;
            if (b.stages & VK_SHADER_STAGE_VERTEX_BIT)
            {
                addDescriptorToUsage(domain_v[domain], b.type, n);
            }
            if (b.stages & VK_SHADER_STAGE_FRAGMENT_BIT)
            {
                addDescriptorToUsage(domain_f[domain], b.type, n);
            }
            if (b.stages & VK_SHADER_STAGE_COMPUTE_BIT)
            {
                addDescriptorToUsage(domain_c[domain], b.type, n);
            }
        }
    }

    const auto mergedFor = [&](const PipelineReflectedInfo* refl, uint64_t owner)
    {
        if (!refl || refl->slots.empty())
        {
            return;
        }

        bool touches[4] = {false, false, false, false};
        LayoutPlan::StageUsage v{}, f{}, c{};
        for (const auto& slot : refl->slots)
        {
            const auto d = static_cast<std::size_t>(slotFrequency(slot));
            if (slot.source == ESlotSource::PipelinePrivate)
            {
                // Private sets don't participate in merging; count only their
                // own share — using the same bucketing rule as the other two
                // tally sites (this one used to only accumulate the total,
                // which skewed the per-type readings).
                const PlannedSet* ps = plan.find(slot.slot, EPlannedSetKind::PipelinePrivate, owner);
                if (!ps)
                {
                    continue;
                }
                for (const auto& b : ps->bindings)
                {
                    if (b.binding_flags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT)
                    {
                        continue;
                    }
                    const uint32_t n = b.count == 0 ? 1u : b.count;
                    if (b.stages & VK_SHADER_STAGE_VERTEX_BIT)
                    {
                        addDescriptorToUsage(v, b.type, n);
                    }
                    if (b.stages & VK_SHADER_STAGE_FRAGMENT_BIT)
                    {
                        addDescriptorToUsage(f, b.type, n);
                    }
                    if (b.stages & VK_SHADER_STAGE_COMPUTE_BIT)
                    {
                        addDescriptorToUsage(c, b.type, n);
                    }
                }
                continue;
            }
            touches[d] = true;
        }
        const auto addDomain = [](LayoutPlan::StageUsage& dst, const LayoutPlan::StageUsage& s) {
            dst.uniform_buffer += s.uniform_buffer;
            dst.storage_buffer += s.storage_buffer;
            dst.sampled_image  += s.sampled_image;
            dst.storage_image  += s.storage_image;
            dst.total          += s.total;
        };
        for (std::size_t d = 0; d < 4; ++d)
            if (touches[d]) { addDomain(v, domain_v[d]); addDomain(f, domain_f[d]); addDomain(c, domain_c[d]); }

        const std::pair<const char*, const LayoutPlan::StageUsage*> ms[] = {
            {"VERTEX", &v}, {"FRAGMENT", &f}, {"COMPUTE", &c},
        };
        for (const auto& [nm, u] : ms)
            if (u->total > plan.worst_stage_merged.total)
            {
                plan.worst_stage_merged      = *u;
                plan.worst_stage_merged_name = nm;
            }
    };

    for (const auto& pass : compiled.original_graph.passes)
    {
        if (pass.pipeline_template.valid())
            mergedFor(pipeline_manager.templateReflection(pass.pipeline_template),
                      graphicsOwnerKey(pass.pipeline_template.index));
        for (const auto& extra : pass.additional_pipelines)
            if (extra.valid())
            {
                mergedFor(pipeline_manager.templateReflection(extra), graphicsOwnerKey(extra.index));
            }
        if (pass.compute_pipeline_handle.valid())
            mergedFor(pipeline_manager.computeReflection(pass.compute_pipeline_handle),
                      computeOwnerKey(pass.compute_pipeline_handle.index));
    }

    // The summary line takes whichever stage has the heaviest usage
    // (per-pipeline violation detail was already recorded in accumulate).
    const std::pair<const char*, const LayoutPlan::StageUsage*> stages[] = {
        {"VERTEX", &vertex}, {"FRAGMENT", &fragment}, {"COMPUTE", &compute},
    };
    for (const auto& [name, u] : stages)
    {
        if (u->total > plan.worst_stage.total)
        {
            plan.worst_stage      = *u;
            plan.worst_stage_name = name;
        }
    }

    compiled.layout_plan = std::move(plan);
    return true;
}

} // namespace lux::render
