#pragma once
// =============================================================================
//  LayoutPlan.hpp — the output of descriptor-layout allocation at graph-compile
//  time.
// -----------------------------------------------------------------------------
//  RenderGraphCompiler's computeGraphDescriptorLayouts stage walks the whole
//  graph — pass -> pipeline template -> per-binding reflection — and
//  aggregates the union across the whole graph to determine each set's full
//  shape and slot assignment, producing this structure. It is the single
//  source of truth for three consumers:
//    (1) pipeline layout — finalizeTemplateLayout writes it back to the template;
//    (2) shader-side positions — SpirvPatcher patches canonical SPIR-V to the
//        Plan's slots;
//    (3) set-instance allocation — resource objects allocate their
//        VkDescriptorSet using the Plan's layout.
//
//  Why the union across the whole graph is required: a single pipeline's
//  reflection only contains the subset of bindings it actually uses, but the
//  set instance is allocated with the full shape; binding the full set with a
//  subset layout triggers VUID-vkCmdBindDescriptorSets-00358. This is exactly
//  why the "contract fallback / hole fallback" mechanisms existed earlier in
//  the migration — once the whole-graph union is in place, they can be retired.
//
//  Design: .internal/lux-engine-descriptor-layout-architecture.md §4.1/§4.2
// =============================================================================

#include <lux/engine/description/LayoutContract.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp> // RenderError(warnings)

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

#include <lux/cxx/container/SmallVector.hpp>

namespace lux::render
{
    /// One binding entry in the Plan (its final shape after the whole-graph
    /// union).
    struct PlannedBinding
    {
        uint32_t binding{0};
        VkDescriptorType type{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
        uint32_t count{1};
        VkShaderStageFlags stages{0};
        VkDescriptorBindingFlags binding_flags{0};
        /// Logical resource name (the contract reconciliation key; for
        /// pass-local resources this is the variable name as seen by
        /// reflection).
        std::string name;
    };

    /// The ownership category of a set — determines who decides its shape,
    /// and whether it can be merged across pipelines. These three categories
    /// map one-to-one onto the "three set-ownership kinds" in the design
    /// document, §3.
    enum class EPlannedSetKind : uint8_t
    {
        /// An engine-shared set (Scene/Instance/Light/...). Its shape comes
        /// from EngineSetShapes, and it does **not** participate in
        /// reflection aggregation — reflection only sees the binding subset
        /// a given pipeline actually uses, and a bindless unbounded array
        /// reflects a different count in different shaders anyway.
        EngineShared,
        /// A feature-owned set shared across pipelines (cull set / cluster
        /// set / compact caster set). Its shape is declared explicitly by
        /// the feature; the allocator only records it, never infers it.
        FeatureExplicit,
        /// A set private to a single pipeline (HDR input / blur / hzb build /
        /// cluster clear). Its shape equals that pipeline's reflection.
        /// **Private sets with the same number in different pipelines are
        /// different sets** (Tonemap's set1 has nothing to do with
        /// Canvas2D's set1), so they are recorded separately per owner.
        PipelinePrivate,
    };

    /// One set within the Plan.
    struct PlannedSet
    {
        /// The update-frequency domain this set belongs to, and its starting
        /// binding offset within that domain's set.
        ///
        /// Why an offset is needed: domain merging folds several engine sets
        /// of the same domain into one set, but each of them still numbers
        /// its own bindings starting at 0 (Light uses 0-10, Material uses
        /// 0-4) — merging them directly would collide. The offset shifts
        /// each one's numbering into a non-overlapping range.
        ///
        /// Why the Plan is the one that assigns it: the offset is part of
        /// "position", which shares its source of truth with the set number
        /// itself — letting each resource object work out its own offset
        /// would scatter layout knowledge back out again.
        ///
        /// `domain_binding_count` is how many bindings this set occupies
        /// within the domain, which can be larger than `bindings.size()`:
        /// template-style sets like Material record only one template
        /// binding, which is expanded to N actual bindings by family count.
        /// Computing the offset from size() would let the next set overlap
        /// it.
        rdesc::EBindFrequency domain{rdesc::EBindFrequency::PASS_LOCAL};
        uint32_t domain_binding_offset{0};
        uint32_t domain_binding_count{0};

        uint32_t set{0}; ///< Slot number within its owning pipeline.
        EPlannedSetKind kind{EPlannedSetKind::PipelinePrivate};
        /// Owning-pipeline identity for PipelinePrivate sets (the graphics
        /// template's or compute pipeline's handle index, plus a category
        /// bit to disambiguate them); always 0 for EngineShared.
        uint64_t owner_key{0};
        rdesc::EBindFrequency frequency{rdesc::EBindFrequency::PASS_LOCAL};
        lux::cxx::SmallVector<PlannedBinding, 8> bindings;
        /// This set's VkDescriptorSetLayout (built/reused during the
        /// allocation stage, consumed by all three parties).
        VkDescriptorSetLayout layout{VK_NULL_HANDLE};
        /// Whether the UPDATE_AFTER_BIND pool flag is needed (true if any
        /// binding carries UAB).
        bool update_after_bind{false};
    };

    // ── Ownership key ────────────────────────────────────────────────────
    //  A PipelinePrivate set's identity is (which pipeline, which slot).
    //  Graphics and compute each get their own high bit domain: the two
    //  handle-index families are unrelated, so without a tag bit they would
    //  collide.
    inline constexpr uint64_t kGraphicsOwnerTag = 1ull << 62;
    inline constexpr uint64_t kComputeOwnerTag = 1ull << 63;

    [[nodiscard]] inline constexpr uint64_t graphicsOwnerKey(uint32_t index) noexcept
    {
        return kGraphicsOwnerTag | index;
    }
    [[nodiscard]] inline constexpr uint64_t computeOwnerKey(uint32_t index) noexcept
    {
        return kComputeOwnerTag | index;
    }

    struct LayoutPlan
    {
        lux::cxx::SmallVector<PlannedSet, 8> sets;

        /// Deterministic hash: the same set of resources always produces the
        /// same Plan. Feeds downstream cache keys (SPIR-V patch output,
        /// pipeline layout, PSO's layout_epoch) so that recompiling the
        /// graph hits the cache for everything that didn't change, avoiding
        /// a PSO avalanche.
        uint64_t plan_hash{0};

        /// How many pipelines' layouts this write-back actually changed.
        /// Should always be 0 under an identity allocation — a non-zero
        /// value means the Plan and the build-time output come from
        /// different sources (currently a bug); once we switch to a real
        /// allocation strategy this becomes the normal reading of "how many
        /// pipelines did this graph move."
        uint32_t templates_relaid{0};

        // ── ≤4-set feasibility projection (measured only, not yet acted on) ──
        //  The hard mobile constraint is maxBoundDescriptorSets (typically 4
        //  on Mali), and it constrains how many sets a *single pipeline*
        //  binds, not how many sets exist across the whole graph. So what we
        //  measure here is "the widest pipeline."
        //
        //  Projection rule: sets in the same frequency domain merge into
        //  one. There are exactly 4 domains (GLOBAL / BINDLESS / FEATURE /
        //  PASS_LOCAL), so the projected upper bound is naturally 4.
        //
        //  MEASURED 2026-07-31 (scene_cycle_stress_test --mobile, the
        //  19-feature editor-shaped scene, session tier asserted = Mobile):
        //
        //      max_sets_current        = 4
        //      max_sets_projected      = 4   (optimistic)
        //      max_sets_projected_strict = 4 (conservative)
        //      over_budget / over_budget_strict = empty
        //      worst_stage = worst_stage_merged = FRAGMENT, 7 descriptors
        //
        //  So the answer the "measure before acting" note below was waiting
        //  for is: the graph ALREADY fits, and it fits under the CONSERVATIVE
        //  accounting — feature-owned sets do not need folding into the
        //  scene-level domain set, so the lifetime coupling that would have
        //  cost us (feature resources hanging off the scene) is not owed.
        //  The merge cost is likewise zero: 7 descriptors on the heaviest
        //  stage either way, against Vulkan's minimum guarantees of
        //  ubo 12 / ssbo 4 / img 16 / simg 4 / total 128.
        //
        //  The original note, kept because the reasoning still governs any
        //  FUTURE change of strategy: measure precisely how many sets we'd
        //  end up with after compaction and whether that's enough, before
        //  touching the allocation strategy. Changing the strategy first and
        //  checking after is backwards — a failure would require rolling back
        //  the shader patches, set-instance rebuild, and record-time binding
        //  together.
        //
        //  Regression net: scene_cycle_stress_test_mobile fails when the
        //  over-budget marker appears in the graph dump.

        /// How many sets the currently-widest pipeline binds (= the set
        /// count of its pipeline layout as things stand today).
        uint32_t max_sets_current{0};
        /// How many sets the widest pipeline would need after merging by
        /// frequency domain (**optimistic accounting**: assumes a feature's
        /// own shared sets can also be folded into the scene-level FEATURE
        /// domain set).
        uint32_t max_sets_projected{0};
        /// Same as above, but under **conservative accounting**: a
        /// feature's own shared sets are *not* folded into the domain set —
        /// they stay as their own set.
        ///
        /// The difference between the two accountings is not academic: a
        /// feature-owned set (the GPU-cull feature's cull set, the
        /// clustered feature's cluster set) is held by the feature and its
        /// lifetime follows the feature, whereas the domain set is
        /// scene-scoped — folding them in would mean hanging the feature's
        /// resources off the scene's lifetime. If the conservative
        /// accounting already fits the budget, there's no need to take that
        /// risk.
        uint32_t max_sets_projected_strict{0};
        // Both lists below are measured against Vulkan's REQUIRED MINIMUM for
        // maxBoundDescriptorSets (4) — never against the running device's
        // limit. Same rule as stage_over_min: a desktop part reports 32, so
        // measuring locally would silence exactly the finding these exist to
        // surface. "Will it run on THIS device" is a different question, asked
        // at a different time, by PipelineLayoutService::getOrCreate.

        /// Pipelines that exceed budget under conservative accounting
        /// (non-empty means feature-owned sets must be merged in to fit).
        std::vector<std::string> over_budget_strict;
        /// Names of pipelines that still exceed the budget after projection
        /// (should be empty; non-empty means domain merging alone isn't
        /// enough and something more aggressive is needed).
        std::vector<std::string> over_budget;

        // ── Per-stage descriptor usage (a separate hard constraint, unrelated to set count) ──
        //  The per-stage limit counts "how many descriptors a given stage
        //  can see," accumulated across *all bound sets* — so merging 8 sets
        //  down to 4 doesn't reduce this number by a single descriptor. It's
        //  a constraint independent of the ≤4-set work, and Vulkan's minimum
        //  guarantee here is quite low (storage buffers only guarantee 4 per
        //  stage), so this could well be the real bottleneck on mobile.
        //
        //  Only counts bindings that are *not* UPDATE_AFTER_BIND: UAB
        //  bindings fall under the maxPerStageDescriptorUpdateAfterBind*
        //  limits instead, which are typically in the millions — the
        //  engine's bindless tables carry UAB precisely for that reason.
        //  Mixing the two into one count would produce a false alarm of
        //  being "tens of thousands of times over budget."
        struct StageUsage
        {
            uint32_t uniform_buffer{0};
            uint32_t storage_buffer{0};
            uint32_t sampled_image{0}; ///< Includes COMBINED_IMAGE_SAMPLER.
            uint32_t storage_image{0};
            uint32_t total{0};
        };
        /// The reading for the single heaviest stage (**before merging**,
        /// i.e. the current state).
        StageUsage worst_stage{};
        /// The same reading after merging.
        ///
        /// Why this must be computed separately: merging sets isn't free —
        /// once several sets in the same domain are folded into one, *every*
        /// pipeline that binds that set "sees" all of its bindings, even if
        /// it only uses two of them. So the set count goes down while the
        /// per-stage descriptor count goes up. Using the pre-merge reading
        /// to argue the post-merge state is safe would be wrong; the two
        /// numbers must be measured independently.
        ///
        /// This is the real cost of the ≤4-set approach, and it's what
        /// determines whether the approach actually holds up.
        StageUsage worst_stage_merged{};
        const char* worst_stage_merged_name{nullptr};
        /// Which stage this is (VERTEX/FRAGMENT/COMPUTE); null means nothing
        /// was measured.
        const char* worst_stage_name{nullptr};
        /// Entries that exceed Vulkan's *minimum guarantee* (not the local
        /// machine's actual limit — the local machine is usually far more
        /// generous, but that doesn't mean the target mobile device is too).
        std::vector<std::string> stage_over_min;

        //  Note: `resource_slot_map` ("which engine set lives in this slot")
        //  holds the same information as the {logical_set, slot} recorded by
        //  ReflectedSlot. Verified consistent everywhere on 2026-07-19, and
        //  auto-synthesis was switched to per-slot classification
        //  derivation, so it is no longer an independent source of truth. If
        //  the two ever diverge again it goes into warnings — that would
        //  mean someone reintroduced a hand-written second copy of the
        //  truth.

        /// Suspicious findings discovered during aggregation (e.g. the same
        /// set declared with different shapes by two pipelines, etc.).
        /// **Not a compile error**: the Plan is currently a descriptive
        /// artifact, and letting it veto the whole graph would be
        /// disproportionate — that was tried once, and the result was that
        /// not a single real-world scene graph would compile. Once the Plan
        /// actually drives layout, each of these will be promoted to a hard
        /// error, but that has to ship together with the fix.
        std::vector<RenderError> warnings;

        // ── Per-pipeline slot table (pure diagnostics, not part of plan_hash) ──
        //  迁移期的观测手段。**注意时态**:当年门禁拒绝一条管线时它会
        //  *静默回落 legacy 路径*、而门禁照样显示绿色 —— 这张表就是为了让
        //  "绿"真正等于"已迁移"。之后回落路径已删除(PipelineManager
        //  对引用引擎契约资源却未标记域合并的管线**直接拒绝注册**并带诊断),
        //  所以这张表的原始动机已经消失;它现在的价值是逐管线呈现槽位配置,
        //  仍然值得留 —— 只是别再照着这段读成"失败会静默"。
        //  逐管线记录是否已切换、每个槽位如何配置,写进 graph
        //  dump. `kind` mirrors ESlotSource with a character code (to avoid
        //  dragging PipelineManager.hpp into this header): E=EngineShared,
        //  X=FeatureExplicit, P=PipelinePrivate, H=ReflectionHole,
        //  D=DomainMerged.
        struct PipelineSlotDiag
        {
            uint32_t slot{0};
            char kind{'P'};
            /// For E/H: the canonical set number (H's sentinel value
            /// denotes a semantically-empty placeholder hole in domain
            /// mode); for D: the domain enum value; for X/P: the slot
            /// number itself.
            uint32_t logical{0};
            /// The canonical set numbers of a D slot's member sets (looked
            /// up from reflection names via the contract, de-duplicated).
            lux::cxx::SmallVector<uint32_t, 8> members;
        };
        struct PipelineDiag
        {
            std::string name;
            bool merged{false}; ///< Already switched to the domain-merged layout (the merged flag).
            bool legacy{false}; ///< A legacy pipeline: brings its own layout, has no slot table.
            /// An identity pipeline: its reflection contains no engine
            /// resource that *would move* (e.g. one that only uses the
            /// Scene's Grid/LineList, or a purely-private Tonemap/Blur).
            /// After patching, the bytes are identical to the source, dedup
            /// hits the source slot, and the merged flag is naturally left
            /// unset — merged or not is physically the same shape here, so
            /// this isn't "not yet migrated"; a retirement audit should
            /// treat it the same as [migrated].
            bool identity{false};
            lux::cxx::SmallVector<PipelineSlotDiag, 8> slots;
        };
        std::vector<PipelineDiag> pipelines;

        /// The final merged position of an engine set: (domain slot number,
        /// starting binding offset within the domain).
        ///
        /// This is the destination SpirvPatcher relocates a shader's
        /// canonical position to — the relocation table is derived from
        /// this entry by entry (see the [Relocations] section of
        /// RGDebugPrint).
        struct MergedPosition
        {
            uint32_t set{0};
            uint32_t binding_offset{0};
        };

        /// Look up by logical identity. For an engine set, just pass its
        /// canonical number (owner_key is always 0).
        [[nodiscard]] const PlannedSet*
        find(uint32_t set, EPlannedSetKind kind = EPlannedSetKind::EngineShared, uint64_t owner_key = 0) const noexcept
        {
            for (const auto& s : sets)
                if (s.set == set && s.kind == kind && s.owner_key == owner_key)
                    return &s;
            return nullptr;
        }
    };

} // namespace lux::render
