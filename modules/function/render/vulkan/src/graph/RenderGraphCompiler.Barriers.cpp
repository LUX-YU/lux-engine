#include <lux/engine/render/graph/RenderGraphCompiler.hpp>
#include <lux/engine/render/core/FrustumCuller.hpp>
#include <lux/engine/render/graph/RGLoadOpPolicy.hpp>
#include <lux/engine/render/graph/KernelDescriptor.hpp>
#include <lux/engine/render/graph/ProgramEmitter.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp>                       // domain slot resolution (kEngineSetShapes)
#include <lux/engine/render/gpu/descriptor/SceneDomainDescriptorSets.hpp> // domain set instance (record-time collapsed binding)
#include <lux/engine/render/graph/RGBarrierUtils.hpp>
#include <lux/engine/render/graph/vk_type_converter.hpp>   // convertVkImageLayout (neutral DS layout)
#include <algorithm>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <span>

namespace lux::render
{
    namespace
    {
        // Returns the appropriate VkImageAspectFlags for a given texture format.
        VkImageAspectFlags buildAspectMask(lux::common::ETextureFormat format) noexcept
        {
            if (isDepthStencilFormat_shared(format))
            {
                VkImageAspectFlags mask = VK_IMAGE_ASPECT_DEPTH_BIT;
                if (hasStencilComponent_shared(format))
                    mask |= VK_IMAGE_ASPECT_STENCIL_BIT;
                return mask;
            }
            return VK_IMAGE_ASPECT_COLOR_BIT;
        }

        VkDescriptorSet resolveFeatureDomainSet(
            const void* resource,
            uint32_t frame_slot,
            uint32_t /*view_id*/)
        {
            return static_cast<const SceneDomainDescriptorSets*>(resource)->set(
                rdesc::EBindFrequency::FEATURE,
                frame_slot);
        }

        VkDescriptorSet resolveGlobalDomainSet(
            const void* resource,
            uint32_t frame_slot,
            uint32_t /*view_id*/)
        {
            return static_cast<const SceneDomainDescriptorSets*>(resource)->set(
                rdesc::EBindFrequency::GLOBAL,
                frame_slot);
        }

    } // namespace

    void RenderGraphCompiler::computeDescriptorBindingPlan(RGCompiledGraph& compiled,
                                                        PipelineManager& pipeline_manager,
                                                        const SceneDomainDescriptorSets* domain_sets)
    {
        const auto& order = compiled.execution_order;
        if (order.empty()) return;

        // The merged domain of a given slot in this pass's pipeline (-1 = not
        // a domain slot, otherwise an rdesc::EBindFrequency enum value). Both
        // the graphics and compute paths need this lookup — compute pipelines
        // (cluster lighting, skinning) also switch over to the domain layout.
        const auto domainOfSlot = [&pipeline_manager](
            const RGPassDescription& pass, uint32_t slot_index) -> int
        {
            const PipelineReflectedInfo* refl = nullptr;
            if (pass.pipeline_template.valid())
                refl = pipeline_manager.templateReflection(pass.pipeline_template);
            else if (pass.compute_pipeline_handle.valid())
                refl = pipeline_manager.computeReflection(pass.compute_pipeline_handle);
            if (!refl) return -1;
            for (const auto& rslot : refl->slots)
                if (rslot.slot == slot_index)
                    return rslot.source == ESlotSource::DomainMerged
                         ? static_cast<int>(rslot.logical_set) : -1;
            return -1;
        };

        constexpr uint32_t kMaxSlots = 16;
        std::array<VkPipelineLayout, kMaxSlots> slot_last_layout{};
        std::array<uint64_t,         kMaxSlots> slot_last_identity{};
        slot_last_layout.fill(VK_NULL_HANDLE);
        slot_last_identity.fill(0ull);

        auto resetSlotTracking = [&]() {
            slot_last_layout.fill(VK_NULL_HANDLE);
            slot_last_identity.fill(0ull);
        };

        // Logical identity -> the real slot number in this pipeline.
        //
        // This looks up the template's resource_slot_map, which has been
        // changed to be derived from ReflectedSlot (see
        // PipelineManager::registerGraphicsTemplate) — it shares the same
        // source as the pipeline layout, so "the bound set is compatible with
        // that slot's layout" is guaranteed by construction, not a
        // coincidence.
        //
        // Sentinel meaning "this pipeline doesn't use this set" — greater
        // than kMaxSlots, so the caller's `if (slot >= kMaxSlots) continue;`
        // naturally skips this binding.
        constexpr uint32_t kSlotUnbound = ~0u;

        // Both the graphics and compute paths need this resolved. A compute
        // pipeline has no resource_slot_map (that's a graphics-template
        // field), but it likewise keeps a ReflectedSlot slot table — look it
        // up directly. This matters especially on the compute side: skinning
        // puts the vertex pool at set 1 while its canonical number is 7, and
        // without resolving this you'd be stuck hand-writing bare slot
        // numbers.
        //
        // When resolution fails, fall back to the binding's own canonical
        // initial value (no logical set, a legacy-path pipeline supplying its
        // own layout, or this slot not being registered under an engine
        // identity) — matching pre-migration behavior.
        const auto resolveLogicalSlot = [&pipeline_manager](
            const PassDSBinding& binding, const RGPassDescription& pass) -> uint32_t
        {
            if (!binding.logical.has_value())
            {
                // A bare slot literal is translated through the PRIVATE
                // REMAP TABLE (the real allocation strategy moves private
                // sets that are >3 or collide with a domain slot, e.g. mesh's
                // visible set 5 -> 3). If the table is empty (not switched
                // over / nothing to remap), return it unchanged, matching
                // pre-migration behavior.
                const PipelineReflectedInfo* refl = nullptr;
                if (pass.pipeline_template.valid())
                    refl = pipeline_manager.templateReflection(pass.pipeline_template);
                else if (pass.compute_pipeline_handle.valid())
                    refl = pipeline_manager.computeReflection(pass.compute_pipeline_handle);
                if (refl)
                    for (const auto& [from, to] : refl->private_set_remap)
                        if (from == binding.slot)
                            return to;
                return binding.slot;
            }

            if (pass.pipeline_template.valid())
            {
                const auto& map =
                    pipeline_manager.getTemplate(pass.pipeline_template).resource_slot_map;
                for (const auto& [logical, index] : map)
                    if (logical == *binding.logical)
                        return index;

                // Reaching here means one of two very different situations,
                // and they must be handled separately — conflating them gets
                // both wrong:
                //
                //  (1) NO SLOT TABLE AT ALL: a legacy-path pipeline (caller
                //      supplies its own pipeline_layout, buildReflectedPipelineLayout
                //      never ran). It uses the unmerged standard layout, so
                //      the canonical slot is correct -> fall back to it.
                //      Mistaking this for case (2) would make it bind
                //      nothing at all: observed in practice as "uses set #1
                //      but that set is not bound".
                //
                //  (2) HAS A SLOT TABLE BUT NO ENTRY FOR THIS ONE: this
                //      pipeline simply doesn't use this engine set. Falling
                //      back to the canonical slot happens to be harmless
                //      under the unmerged layout (that slot was already its
                //      own), but after domain merging the slots are
                //      reshuffled and the canonical slot now belongs to
                //      someone else -> don't bind. Mistaking this for case
                //      (1) causes a wrong bind: observed in practice as
                //      Skybox not using Instance yet still getting it bound —
                //      after merging, Instance's canonical slot 1 became the
                //      texture table's slot, binding a 2-descriptor set onto
                //      a bindless layout with millions of entries
                //      (VUID-00358).
                return map.empty() ? binding.slot : kSlotUnbound;
            }

            if (pass.compute_pipeline_handle.valid())
            {
                if (const auto* refl =
                        pipeline_manager.computeReflection(pass.compute_pipeline_handle))
                {
                    const auto want = static_cast<uint32_t>(*binding.logical);
                    for (const auto& slot : refl->slots)
                    {
                        // Merged domain slot: match members by domain
                        // membership (want's frequency domain == this slot's
                        // domain). Compute pipelines don't currently
                        // participate in merging; this branch exists only
                        // for completeness.
                        if (slot.source == ESlotSource::DomainMerged)
                        {
                            if (want < kEngineSetShapes.size() &&
                                static_cast<uint32_t>(kEngineSetShapes[want].frequency) ==
                                    slot.logical_set)
                                return slot.slot;
                            continue;
                        }
                        if (slot.source != ESlotSource::EngineShared &&
                            slot.source != ESlotSource::ReflectionHole)
                            continue;
                        if (slot.logical_set == want)
                            return slot.slot;
                    }
                }
            }
            return binding.slot;
        };

        for (const uint32_t idx : order)
        {
            RGCompiledPass& cpass = compiled.compiled_passes[idx];
            const VkPipelineLayout curr_layout = cpass.render.pipeline_layout;
            cpass.render.ds_bind_recipe.clear();

            // Render-pass boundary: Vulkan spec requires all DS bindings to be
            // re-established after vkCmdBeginRenderPass / vkCmdEndRenderPass.
            if (cpass.render.begin_render_pass)
                resetSlotTracking();

            uint32_t bind_mask = 0;
            // Slots for which this pass has already emitted a domain bind
            // (collapsing: multiple logical bindings -> one domain bind).
            uint32_t domain_emitted_mask = 0;

            for (const PassDSBinding& binding : cpass.pass->ds_bindings)
            {
                // Logical identity -> the real slot number. The same engine
                // set can sit at a different slot in different pipelines
                // (skinning's vertex pool at set 1, the shadow compact layout
                // at set 3), so this must be resolved against THIS PASS'S
                // PIPELINE's slot table, not by canonical number. The slot
                // table itself is recorded in place by
                // buildReflectedPipelineLayout (ReflectedSlot) and shares the
                // same source as the pipeline layout — that's exactly the
                // guarantee that "the bound set is necessarily compatible
                // with that slot's layout". When resolution fails (a
                // legacy-path pipeline supplying its own layout, no
                // reflection slot table), fall back to the binding's own
                // canonical initial value, matching pre-migration behavior.
                const uint32_t slot = resolveLogicalSlot(binding, *cpass.pass);
                if (slot >= kMaxSlots) continue;

                // -- Domain-slot collapsing (domain merging) ------------------
                //
                // In a merged pipeline, the logical bindings for
                // Instance/Light/Material/... all resolve to the same domain
                // slot, and the per-owner, per-set instances they carry are
                // incompatible with the merged domain layout — the
                // FEATURE/GLOBAL slots only recognize the domain instance
                // from SceneDomainDescriptorSets (a dual write keeps its data
                // consistent with the per-set version); the BINDLESS domain
                // "instance" is simply the global texture table the binding
                // already carries, so its original recipe is kept and only
                // the rebind is forced. The first logical binding emits one
                // domain bind; subsequent bindings to the same slot are
                // skipped.
                const int slot_domain = binding.logical.has_value()
                    ? domainOfSlot(*cpass.pass, slot) : -1;
                if (slot_domain == static_cast<int>(rdesc::EBindFrequency::FEATURE) ||
                    slot_domain == static_cast<int>(rdesc::EBindFrequency::GLOBAL))
                {
                    if (!domain_sets)
                    {
                        if (compiled.layout_plan.has_value())
                            compiled.layout_plan->warnings.push_back(
                                renderError<err::graph::DomainSlotWithoutDomainSets>(cpass.pass_index));
                        continue;
                    }
                    if (domain_emitted_mask & (1u << slot))
                        continue;
                    domain_emitted_mask |= (1u << slot);

                    DSBindRecipe domain_recipe{};
                    domain_recipe.slot     = slot;
                    domain_recipe.resolve  = &DSBindRecipe::resolveResource;
                    domain_recipe.provider = DescriptorProvider{
                        domain_sets,
                        slot_domain == static_cast<int>(rdesc::EBindFrequency::GLOBAL)
                            ? &resolveGlobalDomainSet : &resolveFeatureDomainSet};
                    cpass.render.ds_bind_recipe.push_back(domain_recipe);

                    // FORCE A REBIND ON EVERY PASS, NO CROSS-PASS DEDUP — the
                    // same semantics as the PER_FIF binding this collapsing
                    // replaced. Dedup caused a real incident here once: the
                    // compile-time slot tracker doesn't understand Vulkan's
                    // layout-compatibility disturbance rule (an intervening
                    // pass binding ANY set with a layout that has a different
                    // push-constant range disturbs EVERY already-bound set —
                    // the cluster pipeline's PrefixScan disturbed
                    // ClusterFill's domain slot exactly this way, and dedup
                    // let Fill skip its rebind, producing VUID-...-08600).
                    // The cost of one extra vkCmdBindDescriptorSets call is
                    // negligible; correctness comes first.
                    bind_mask |= (1u << slot);
                    slot_last_layout[slot]   = curr_layout;
                    slot_last_identity[slot] = static_cast<uint64_t>(
                        reinterpret_cast<uintptr_t>(domain_sets));
                    continue;
                }

                DSBindRecipe recipe{};
                recipe.slot = slot;
                switch (binding.source)
                {
                case EDSBindingSource::Immutable:
                    recipe.resolve = &DSBindRecipe::resolveImmutable;
                    recipe.immutable_set = binding.immutable_set;
                    break;
                case EDSBindingSource::Scene:
                    recipe.resolve = &DSBindRecipe::resolveScene;
                    break;
                case EDSBindingSource::Transient:
                    recipe.resolve = &DSBindRecipe::resolveTransient;
                    recipe.transient_ds_index = binding.transient_ds_index;
                    break;
                case EDSBindingSource::Resource:
                    recipe.resolve = &DSBindRecipe::resolveResource;
                    recipe.provider = binding.provider;
                    break;
                case EDSBindingSource::EngineDomain:
                    // 走到这里 = useEngineSet 的域解析没成功:要么该逻辑集不在
                    // FEATURE/GLOBAL 域(BINDLESS 请用 bindImmutableDS),要么本图
                    // 压根没有域实例(scene-less 测试图)。上面的 collapsing 分支
                    // 才是它的正常归宿。机制退休后**没有 per-set 兜底可回落**,
                    // 所以这里不产出 recipe,置告警交给门禁 —— 绑一个错的集比
                    // 不绑更难查。
                    if (compiled.layout_plan.has_value())
                        compiled.layout_plan->warnings.push_back(
                            renderError<err::graph::EngineSetUnresolved>(cpass.pass_index));
                    continue;
                }
                cpass.render.ds_bind_recipe.push_back(recipe);

                const bool force_rebind =
                    (binding.mode == EDSBindMode::PER_FIF)
                 || (binding.mode == EDSBindMode::VERSIONED)
                 || (binding.source == EDSBindingSource::Scene)
                 || (binding.source == EDSBindingSource::Transient)
                 // A domain slot (BINDLESS falls here too: the instance is
                 // whatever global table the binding already carries) follows
                 // the same forced-rebind, no-cross-pass-dedup rule as
                 // FEATURE/GLOBAL.
                 || (slot_domain >= 0);

                uint64_t identity = 0ull;
                if (!force_rebind)
                {
                    if (binding.source == EDSBindingSource::Immutable)
                    {
                        identity = static_cast<uint64_t>(
                            reinterpret_cast<uintptr_t>(binding.immutable_set));
                    }
                    else if (binding.source == EDSBindingSource::Resource)
                    {
                        identity = static_cast<uint64_t>(
                            reinterpret_cast<uintptr_t>(binding.provider.resource));
                    }
                }

                // Layout incompatibility → Vulkan invalidates the slot.
                if (slot_last_layout[slot] != curr_layout)
                {
                    bind_mask |= (1u << slot);
                    slot_last_layout[slot] = curr_layout;
                    slot_last_identity[slot] = identity;
                }
                else if (force_rebind)
                {
                    bind_mask |= (1u << slot);
                }
                else
                {
                    if (slot_last_identity[slot] != identity)
                    {
                        bind_mask |= (1u << slot);
                        slot_last_identity[slot] = identity;
                    }
                }

            }

            cpass.render.bind_ds_mask = bind_mask;

            // After an elective (conditional) pass the runtime binding state is
            // unpredictable — the pass may or may not have executed.  Reset
            // tracking so the next pass conservatively re-binds everything.
            if (cpass.elective_kind != EElectiveKind::NONE)
                resetSlotTracking();
        }
    }

    // ---------------------------
    // 6) Compute Barriers
    // ---------------------------

    void RenderGraphCompiler::computeBarriers(RGCompiledGraph& compiled)
    {
        const uint32_t resource_count = static_cast<uint32_t>(compiled.original_graph.resources.size());

        // 1. Initialize resource state tracking table
        std::vector<ResourceStateTracker> resource_states(resource_count);

        // Initialize Imported resource states
        for (uint32_t i = 0; i < resource_count; ++i) {
            const auto& res = compiled.original_graph.resources[i];
            if (res.lifetime == ERGResourceLifetime::IMPORTED && res.import_info) {
                resource_states[i].current_layout = res.import_info->initial_layout;
                resource_states[i].last_stage_mask = res.import_info->initial_stage;
                resource_states[i].last_access_mask = res.import_info->initial_access;
                // If initial_layout is not UNDEFINED, we consider it already touched,
                // so subsequent barrier calculations will be based on this initial state.
                // If it is UNDEFINED, touched = false will cause a transition from UNDEFINED on first use.
                if (res.import_info->initial_layout != VK_IMAGE_LAYOUT_UNDEFINED) {
                    resource_states[i].touched = true;
                }
            }
        }

        // ── Local-read merged groups: lookups ──────────────────
        // pass → its local_read group (or UINT32_MAX); resource → "input-read
        // attachment of such a group"; group → its first pass (barrier sink).
        std::vector<uint32_t> lr_pass_group(compiled.compiled_passes.size(),
                                            std::numeric_limits<uint32_t>::max());
        std::vector<uint8_t>  lr_input_attachment(resource_count, 0);
        std::vector<uint32_t> lr_group_first_pass(
            compiled.render_pass_layout.groups.size(),
            std::numeric_limits<uint32_t>::max());
        for (uint32_t gi = 0; gi < compiled.render_pass_layout.groups.size(); ++gi)
        {
            const auto& g = compiled.render_pass_layout.groups[gi];
            if (!g.local_read)
                continue;
            lr_group_first_pass[gi] = g.passes.front().pass_index;
            for (const auto& p : g.passes)
            {
                if (p.pass_index < lr_pass_group.size())
                    lr_pass_group[p.pass_index] = gi;
                for (uint32_t s = 0; s < p.input_indices.size(); ++s)
                    if (p.input_indices[s] != VK_ATTACHMENT_UNUSED
                        && g.union_color_res[s] < resource_count)
                        lr_input_attachment[g.union_color_res[s]] = 1;
                if (p.depth_input_index != VK_ATTACHMENT_UNUSED
                    && g.union_depth_res < resource_count)
                    lr_input_attachment[g.union_depth_res] = 1;
            }
        }

        // vkCmdBeginRendering performs an attachment read when loadOp is LOAD.
        // That access is not represented by RGPassTextureRef::usage (a pass may
        // declare a pure WRITE yet preserve content produced by an earlier
        // pass), so recover the compiled loadOp for this exact pass/resource.
        const auto attachmentLoadOp = [&compiled](
            uint32_t pass_idx,
            const RGPassTextureRef& ref) -> VkAttachmentLoadOp
        {
            if (ref.role != lux::common::ETextureRole::COLOR_ATTACHMENT &&
                ref.role != lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
                return VK_ATTACHMENT_LOAD_OP_DONT_CARE;

            const auto& layout = compiled.render_pass_layout;
            if (pass_idx >= layout.pass_to_group.size())
                return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            const uint32_t group_idx = layout.pass_to_group[pass_idx];
            if (group_idx == std::numeric_limits<uint32_t>::max() ||
                group_idx >= layout.groups.size())
                return VK_ATTACHMENT_LOAD_OP_DONT_CARE;

            const auto& group = layout.groups[group_idx];
            if (group.local_read)
            {
                if (ref.role == lux::common::ETextureRole::COLOR_ATTACHMENT)
                {
                    for (std::size_t i = 0; i < group.union_color_res.size(); ++i)
                        if (group.union_color_res[i] == ref.resource.index &&
                            i < group.union_color_load_ops.size())
                            return group.union_color_load_ops[i];
                }
                else if (group.union_depth_res == ref.resource.index)
                {
                    return group.depth_load_op;
                }
                return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            }

            for (const auto& group_pass : group.passes)
            {
                if (group_pass.pass_index != pass_idx)
                    continue;
                if (ref.role == lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
                    return group_pass.pass_depth_load_op;

                const RGPassDescription* pass =
                    pass_idx < compiled.compiled_passes.size()
                        ? compiled.compiled_passes[pass_idx].pass : nullptr;
                if (pass == nullptr)
                    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;

                std::size_t color_ordinal = 0;
                for (const auto& texture : pass->textures)
                {
                    if (texture.role != lux::common::ETextureRole::COLOR_ATTACHMENT)
                        continue;
                    if (texture.resource.index == ref.resource.index)
                        return color_ordinal < group_pass.pass_color_load_ops.size()
                            ? group_pass.pass_color_load_ops[color_ordinal]
                            : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                    ++color_ordinal;
                }
                return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            }
            return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        };

        // 2. Traverse passes in execution order
        for (uint32_t pass_idx : compiled.execution_order)
        {
            RGCompiledPass& cpass = compiled.compiled_passes[pass_idx];
            const RGPassDescription* desc = cpass.pass;

            // Barriers of a NON-FIRST pass inside a local-read group must not
            // land inside the rendering scope — hoist them to the group's
            // first pass (recorded pre-scope). Intra-scope attachment hazards
            // are owned by the LocalReadBoundary by-region barrier instead.
            const uint32_t lr_gi = lr_pass_group[pass_idx];
            const bool lr_hoist = lr_gi != std::numeric_limits<uint32_t>::max()
                && pass_idx != lr_group_first_pass[lr_gi];
            RGCompiledPass& barrier_sink = lr_hoist
                ? compiled.compiled_passes[lr_group_first_pass[lr_gi]]
                : cpass;

            // --- Process Textures ---
            for (const auto& tex_ref : desc->textures)
            {
                const uint32_t res_idx = tex_ref.resource.index;

                // Skip if resource is not valid (e.g., culled)
                if (res_idx >= compiled.valid_resources.size() || !compiled.valid_resources[res_idx]) continue;

                // Get resource description (for determining format, etc.)
                const auto& res_desc_wrap = compiled.original_graph.resources[res_idx];
                const auto& tex_desc = std::get<RGTextureDescription>(res_desc_wrap.desc);

                // Compute target state (pass type selects compute vs fragment
                // shader stage for SAMPLED / UNORDERED_ACCESS).
                VulkanResourceState target_state = determineTextureState(tex_ref, tex_desc, desc->type);
                ResourceStateTracker& tracker = resource_states[res_idx];

                // ── Local-read scope attachments ────────────────
                if (lr_gi != std::numeric_limits<uint32_t>::max()
                    && lr_input_attachment[res_idx])
                {
                    if (tex_ref.role == lux::common::ETextureRole::INPUT_ATTACHMENT)
                    {
                        // Intra-scope read: NO pipeline barrier here (the
                        // LocalReadBoundary command owns by-region sync);
                        // just fold the reader into the tracked state.
                        tracker.last_stage_mask  |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                        tracker.last_access_mask |= VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
                        tracker.current_layout    = VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ;
                        tracker.touched           = true;
                        continue;
                    }
                    // Attachment WRITE of an input-read slot: the whole scope
                    // runs it in RENDERING_LOCAL_READ (write + subpassLoad are
                    // both legal there), so the pre-scope transition targets
                    // that layout directly.
                    target_state.layout = VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ;
                }

                // A LOAD is a real attachment read performed before the pass's
                // shader/raster work. Include it in the barrier destination
                // scope even when the graph declaration itself is WRITE-only.
                // Without this, sync validation reports READ_AFTER_WRITE at
                // vkCmdBeginRendering and the dependency is incomplete.
                if (attachmentLoadOp(pass_idx, tex_ref) == VK_ATTACHMENT_LOAD_OP_LOAD)
                {
                    if (tex_ref.role == lux::common::ETextureRole::COLOR_ATTACHMENT)
                        target_state.access_mask |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
                    else if (tex_ref.role == lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
                        target_state.access_mask |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
                }

                // Check if a Barrier is needed
                // Conditions: Layout mismatch, or previous/current write access (WAW, RAW, WAR conflicts),
                // or first use (requires initial layout transition)
                bool layout_mismatch = (tracker.current_layout != target_state.layout);
                bool need_barrier = layout_mismatch ||
                                    isWriteAccess(tracker.last_access_mask) ||
                                    isWriteAccess(target_state.access_mask) ||
                                    (!tracker.touched); // First use always transitions Layout

                if (need_barrier)
                {
                    // Build sync2 barrier with per-barrier stage masks
                    RGBarrierInfo2 barrier2{};
                    barrier2.resource_index = tex_ref.resource.index;
                    barrier2.srcStageMask   = tracker.last_stage_mask;
                    barrier2.srcAccessMask  = tracker.last_access_mask;
                    barrier2.dstStageMask   = target_state.stage_mask;
                    barrier2.dstAccessMask  = target_state.access_mask;
                    barrier2.oldLayout      = tracker.current_layout;
                    barrier2.newLayout      = target_state.layout;

                    // First touch of an image whose old layout is UNDEFINED.
                    //
                    // UNDEFINED discards the existing contents, so there is nothing to
                    // make visible: srcAccessMask = NONE is correct and stays.
                    //
                    // The EXECUTION dependency is a different question and must NOT be
                    // dropped. Whoever held the image before the graph took it may still
                    // be READING it — the presentation engine between acquire and the
                    // semaphore signal, or the previous frame for any image shared across
                    // frames-in-flight. Overwriting it is a write-after-read, and WAR is
                    // ordered by srcStageMask alone (no memory dependency needed).
                    //
                    // Collapsing srcStageMask to TOP_OF_PIPE waits for nothing, which is
                    // exactly the hazard synchronization validation reports as
                    // SYNC-HAZARD-WRITE-AFTER-READ / SYNC_IMAGE_LAYOUT_TRANSITION vs
                    // PRESENT_ACQUIRE_READ. Keep the handover stage the import declared
                    // (RGImportedResourceInfo::initial_stage, seeded into the tracker
                    // above); it defaults to TOP_OF_PIPE, so transients and imports that
                    // genuinely have no prior reader behave exactly as before.
                    if (!tracker.touched && tracker.current_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                        barrier2.srcAccessMask = VK_ACCESS_2_NONE;
                    }

                    // Subresource range (Mip/Layer)
                    barrier2.subresourceRange.aspectMask = buildAspectMask(tex_desc.format);
                    barrier2.subresourceRange.baseMipLevel   = tex_ref.range.base_mip_level;
                    barrier2.subresourceRange.levelCount     = tex_ref.range.level_count;
                    barrier2.subresourceRange.baseArrayLayer = tex_ref.range.base_array_layer;
                    barrier2.subresourceRange.layerCount     = tex_ref.range.layer_count;

                    barrier_sink.sync.image_barriers_2.push_back(barrier2);
                }

                // Update tracker state. When a barrier WAS emitted it chains the
                // prior scope, so replace. When NO barrier was emitted (read after
                // read in the same layout) ACCUMULATE the reader stage/access:
                // otherwise a later writer's WAR barrier would only wait on the
                // most recent reader and could overlap an earlier reader at a
                // different stage. (Layout is unchanged on the no-barrier path.)
                if (need_barrier)
                {
                    tracker.current_layout   = target_state.layout;
                    tracker.last_stage_mask  = target_state.stage_mask;
                    tracker.last_access_mask = target_state.access_mask;
                }
                else
                {
                    tracker.last_stage_mask  |= target_state.stage_mask;
                    tracker.last_access_mask |= target_state.access_mask;
                }
                tracker.touched = true;
            }

            // --- Process Buffers ---
            for (std::size_t buffer_ref_index = 0u;
                 buffer_ref_index < desc->buffers.size();
                 ++buffer_ref_index)
            {
                const auto& buf_ref = desc->buffers[buffer_ref_index];
                const uint32_t res_idx = buf_ref.resource.index;
                if (res_idx >= compiled.valid_resources.size() ||
                    !compiled.valid_resources[res_idx])
                {
                    continue;
                }

                // One pass may consume the same buffer through several Vulkan
                // domains (for example TerrainGBuffer reads its selection data
                // in the vertex shader and the leading draw arguments at
                // DRAW_INDIRECT).  Process that resource once with the union of
                // every role.  Sequentially treating the second declaration as
                // read-after-read loses the original producer -> second-stage
                // dependency and allowed vkCmdDrawIndirect to observe a buffer
                // that was still being written by compute.
                bool already_merged = false;
                for (std::size_t earlier = 0u;
                     earlier < buffer_ref_index;
                     ++earlier)
                {
                    if (desc->buffers[earlier].resource.index == res_idx)
                    {
                        already_merged = true;
                        break;
                    }
                }
                if (already_merged)
                    continue;

                VulkanResourceState target_state = determineBufferState_shared(
                    buf_ref,
                    desc->type
                );
                std::uint64_t barrier_offset = buf_ref.offset;
                std::uint64_t barrier_end = buf_ref.offset;
                bool whole_buffer = buf_ref.size == 0u;
                if (!whole_buffer)
                {
                    barrier_end = buf_ref.offset >
                        std::numeric_limits<std::uint64_t>::max() -
                            buf_ref.size
                        ? std::numeric_limits<std::uint64_t>::max()
                        : buf_ref.offset + buf_ref.size;
                }
                for (std::size_t sibling_index = buffer_ref_index + 1u;
                     sibling_index < desc->buffers.size();
                     ++sibling_index)
                {
                    const auto& sibling = desc->buffers[sibling_index];
                    if (sibling.resource.index != res_idx)
                        continue;
                    const auto sibling_state = determineBufferState_shared(
                        sibling,
                        desc->type
                    );
                    target_state.stage_mask |= sibling_state.stage_mask;
                    target_state.access_mask |= sibling_state.access_mask;
                    if (whole_buffer || sibling.size == 0u)
                    {
                        whole_buffer = true;
                        continue;
                    }
                    barrier_offset = (std::min)(
                        barrier_offset,
                        sibling.offset
                    );
                    const auto sibling_end = sibling.offset >
                        std::numeric_limits<std::uint64_t>::max() -
                            sibling.size
                        ? std::numeric_limits<std::uint64_t>::max()
                        : sibling.offset + sibling.size;
                    barrier_end = (std::max)(barrier_end, sibling_end);
                }
                ResourceStateTracker& tracker = resource_states[res_idx];

                // Use shared isWriteAccess() instead of duplicating the lambda
                bool last_is_write    = isWriteAccess(tracker.last_access_mask);
                bool current_is_write = isWriteAccess(target_state.access_mask);

                const bool was_touched = tracker.touched;
                bool need_barrier = false;
                if (tracker.touched) {
                    if (last_is_write || current_is_write) {
                        need_barrier = true;
                    }
                }

                if (need_barrier)
                {
                    // Build sync2-style buffer barrier with per-barrier stage masks
                    RGBarrierInfo2 buf_barrier2{};
                    buf_barrier2.resource_index = buf_ref.resource.index;
                    buf_barrier2.srcStageMask   = tracker.last_stage_mask;
                    buf_barrier2.srcAccessMask  = tracker.last_access_mask;
                    buf_barrier2.dstStageMask   = target_state.stage_mask;
                    buf_barrier2.dstAccessMask  = target_state.access_mask;
                    buf_barrier2.oldLayout      = VK_IMAGE_LAYOUT_UNDEFINED;
                    buf_barrier2.newLayout      = VK_IMAGE_LAYOUT_UNDEFINED;
                    buf_barrier2.offset = whole_buffer
                        ? 0u
                        : barrier_offset;
                    buf_barrier2.size = whole_buffer
                        ? VK_WHOLE_SIZE
                        : barrier_end - barrier_offset;
                    barrier_sink.sync.buffer_barriers_2.push_back(buf_barrier2);
                }

                // Same WAR-accumulation rule as textures: on a read-after-read
                // with no barrier, OR the stage/access so a later writer waits on
                // every prior reader. First touch and barrier-emitting accesses
                // replace. (Buffers have no layout to track.)
                //
                // 条件 pass 的纯读例外:不"替换"而是"累积"。替换会把写者
                // 状态从 tracker 冲掉,让后续无条件读者静态上拿不到 barrier
                // ——运行期链跳过时该条件读的 barrier 不执行,写→读同步就
                // 断了(如 Skinning 写 SkinnedVertexPool → HighlightMask 条件
                // 读吞掉了 GBufferDraw 的读 barrier)。累积保留写位,后续
                // 读者也生成 barrier:链跑时 buffer barrier 重复幂等,链跳
                // 时正确同步。
                const bool elective_pure_read =
                    desc->condition && !current_is_write;
                if ((need_barrier || !was_touched) && !elective_pure_read)
                {
                    tracker.last_stage_mask  = target_state.stage_mask;
                    tracker.last_access_mask = target_state.access_mask;
                }
                else
                {
                    tracker.last_stage_mask  |= target_state.stage_mask;
                    tracker.last_access_mask |= target_state.access_mask;
                }
                tracker.touched = true;
            }
        }

        // 3. Handle final layout transitions for imported resources
        for (uint32_t i = 0; i < resource_count; ++i) {
            const auto& res = compiled.original_graph.resources[i];
            if (res.lifetime == ERGResourceLifetime::IMPORTED && res.import_info) {
                // Check if transition to final_layout is needed.
                // 槽位导入(target 附件)恒发射:final_layout 已运行时参数化
                //,编译期恒等(current==final)不代表运行时恒等——
                // 换 target 录制时录制器要有末屏障可补丁;恒等时多付一个
                // 自转换屏障,合法且廉价。非槽位导入维持按需发射。
                const bool is_slot_import = res.import_info->slot.has_value();
                if (res.import_info->final_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
                    (is_slot_import ||
                     resource_states[i].current_layout != res.import_info->final_layout))
                {
                    // Create a Final Barrier Info (sync2)
                    RGBarrierInfo2 final_b2{};
                    final_b2.resource_index = i;
                    final_b2.srcStageMask   = resource_states[i].last_stage_mask;
                    final_b2.srcAccessMask  = resource_states[i].last_access_mask;
                    final_b2.dstStageMask   = res.import_info->final_stage;
                    final_b2.dstAccessMask  = res.import_info->final_access;
                    final_b2.oldLayout      = resource_states[i].current_layout;
                    final_b2.newLayout      = res.import_info->final_layout;

                    if (i < compiled.valid_resources.size() && compiled.valid_resources[i]) {
                        const auto& tex_desc = std::get<RGTextureDescription>(res.desc);
                        final_b2.subresourceRange.aspectMask     = buildAspectMask(tex_desc.format);
                        final_b2.subresourceRange.baseMipLevel   = 0;
                        final_b2.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
                        final_b2.subresourceRange.baseArrayLayer = 0;
                        final_b2.subresourceRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;

                        compiled.final_barrier_infos_2.push_back(final_b2);
                    }
                }
            }
        }

        // 4. Store compile-time final states for imported resources (cross-view sync).
        //    After the final barriers above, each imported resource is in a
        //    deterministic state.  Multi-view recording needs this to patch the
        //    first-touch barrier of view N>0 so its src stage/access/layout
        //    reflects the previous view's completion rather than the declared
        //    initial state.
        //
        //    Update resource_states to account for final barriers:
        for (const auto& fb : compiled.final_barrier_infos_2) {
            resource_states[fb.resource_index].current_layout    = fb.newLayout;
            resource_states[fb.resource_index].last_stage_mask   = fb.dstStageMask;
            resource_states[fb.resource_index].last_access_mask  = fb.dstAccessMask;
        }

        compiled.imported_final_state_lut.resize(resource_count, RGCompiledGraph::kInvalidSlotIdx);
        for (uint32_t i = 0; i < resource_count; ++i) {
            const auto& res = compiled.original_graph.resources[i];
            if (res.lifetime != ERGResourceLifetime::IMPORTED) continue;
            if (!resource_states[i].touched) continue;

            ImportedResourceFinalState fs{};
            fs.resource_index = i;
            fs.stage_mask     = resource_states[i].last_stage_mask;
            fs.access_mask    = resource_states[i].last_access_mask;
            fs.layout         = resource_states[i].current_layout;

            const uint32_t idx = static_cast<uint32_t>(compiled.imported_final_states.size());
            compiled.imported_final_states.push_back(fs);
            compiled.imported_final_state_lut[i] = idx;
        }

    }

    // ---------------------------
    // 6.6) Build pre-built VkBarrier arrays
    // ---------------------------
    // Fills VkImageMemoryBarrier2 / VkBufferMemoryBarrier2 arrays with every static
    // field from the corresponding RGBarrierInfo2.  At record time the loop reduces
    // to a single handle-patch pass (1 store per barrier) followed by vkCmdPipelineBarrier2.
    static VkImageMemoryBarrier2 makeImageBarrier2(const RGBarrierInfo2& info)
    {
        VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask        = info.srcStageMask;
        b.srcAccessMask       = info.srcAccessMask;
        b.dstStageMask        = info.dstStageMask;
        b.dstAccessMask       = info.dstAccessMask;
        b.oldLayout           = info.oldLayout;
        b.newLayout           = info.newLayout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange    = info.subresourceRange;
        b.image               = VK_NULL_HANDLE; // patched per frame
        return b;
    }

    static VkBufferMemoryBarrier2 makeBufferBarrier2(const RGBarrierInfo2& info)
    {
        VkBufferMemoryBarrier2 b{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        b.srcStageMask        = info.srcStageMask;
        b.srcAccessMask       = info.srcAccessMask;
        b.dstStageMask        = info.dstStageMask;
        b.dstAccessMask       = info.dstAccessMask;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.offset              = info.offset;
        b.size                = info.size;
        b.buffer              = VK_NULL_HANDLE; // patched per frame
        return b;
    }

    void RenderGraphCompiler::buildPrebuiltBarriers(RGCompiledGraph& compiled)
    {
        for (auto& cpass : compiled.compiled_passes)
        {
            auto& s = cpass.sync;
            s.prebuilt_image_barriers.clear();
            s.image_patch_resource_idx.clear();
            s.prebuilt_buffer_barriers.clear();
            s.buffer_patch_resource_idx.clear();

            s.prebuilt_image_barriers.reserve(s.image_barriers_2.size());
            s.image_patch_resource_idx.reserve(s.image_barriers_2.size());
            for (const auto& info : s.image_barriers_2)
            {
                s.prebuilt_image_barriers.push_back(makeImageBarrier2(info));
                s.image_patch_resource_idx.push_back(info.resource_index);
            }

            s.prebuilt_buffer_barriers.reserve(s.buffer_barriers_2.size());
            s.buffer_patch_resource_idx.reserve(s.buffer_barriers_2.size());
            for (const auto& info : s.buffer_barriers_2)
            {
                s.prebuilt_buffer_barriers.push_back(makeBufferBarrier2(info));
                s.buffer_patch_resource_idx.push_back(info.resource_index);
            }

        }

        // Final / export barriers
        compiled.prebuilt_final_barriers.clear();
        compiled.final_patch_resource_idx.clear();
        compiled.prebuilt_final_barriers.reserve(compiled.final_barrier_infos_2.size());
        compiled.final_patch_resource_idx.reserve(compiled.final_barrier_infos_2.size());
        for (const auto& info : compiled.final_barrier_infos_2)
        {
            compiled.prebuilt_final_barriers.push_back(makeImageBarrier2(info));
            compiled.final_patch_resource_idx.push_back(info.resource_index);
        }

        // B2+B4: Free compile-time RGBarrierInfo2 vectors — only prebuilt arrays
        // are used at record time.
        for (auto& cpass : compiled.compiled_passes)
        {
            auto& s = cpass.sync;
            s.image_barriers_2              = {};
            s.buffer_barriers_2             = {};
            s.release_ownership_barriers    = {};
            s.acquire_ownership_barriers    = {};
        }
        compiled.final_barrier_infos_2 = {};
    }

    // ---------------------------
    // 7) Compute Descriptor Sets
    // ---------------------------
    void RenderGraphCompiler::computeDescriptorSets(RGCompiledGraph& compiled, PipelineManager& pipeline_manager)
    {
        for (uint32_t pass_idx : compiled.execution_order)
        {
            RGCompiledPass& cpass = compiled.compiled_passes[pass_idx];
            cpass.resources.pass_resource_bindings.clear();

            // DESIGN-01: Handle both GRAPHICS and COMPUTE passes
            if (cpass.pass->type != ERGPassType::GRAPHICS &&
                cpass.pass->type != ERGPassType::COMPUTE &&
                cpass.pass->type != ERGPassType::ASYNC_COMPUTE) continue;

            if (!cpass.pass->pipeline_template.valid())
            {
                // Compute pass without a graphics pipeline_template.
                // pass_resource_bindings stays empty — the processor owns its own
                // descriptor binding in processGroup().
                continue;
            }

            // Graphics pass with a registered pipeline template.
            const auto& tmpl = pipeline_manager.getTemplate(cpass.pass->pipeline_template);

            if (!tmpl.resource_slot_map.empty())
            {
                // Authoritative path: copy resource_slot_map from the template.
                // Populated by PipelineManager::registerGraphicsTemplate from shader
                // reflection; each entry is (EDescriptorSetSlot, vk_slot_index).
                cpass.resources.pass_resource_bindings = tmpl.resource_slot_map;
            }
            else
            {
                // Legacy template without resource_slot_map (pre-reflection).
                // Synthesise identity (slot_name == vk_index) entries from active_sets_mask.
                const uint32_t mask = (tmpl.active_sets_mask != 0)
                    ? tmpl.active_sets_mask : kAllSetsMask;
                for (uint32_t i = 0; i < kDescriptorSetCount; ++i)
                {
                    if ((mask & (1u << i)) && mapSetIndexToResourceType(i).has_value())
                        cpass.resources.pass_resource_bindings.push_back(
                            {static_cast<EDescriptorSetSlot>(i), i});
                }
            }
        }
    }




} // namespace lux::render
