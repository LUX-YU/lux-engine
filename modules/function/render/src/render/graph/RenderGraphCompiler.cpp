#include <lux/engine/render/graph/RenderGraphCompiler.hpp>
#include <lux/engine/render/graph/RGLoadOpPolicy.hpp>
#include <lux/engine/render/graph/KernelDescriptor.hpp>
#include <lux/engine/render/graph/ProgramEmitter.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/graph/RGBarrierUtils.hpp>
#include <lux/engine/render/graph/vk_type_converter.hpp>   // convertVkImageLayout (neutral DS layout)
#include <lux/engine/render/graph/GpuDrivenMeshConsts.hpp>
#include <lux/engine/render/resources/mesh/MdcTable.hpp>
#include <algorithm>
#include <cstdio>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <span>

namespace lux::render
{

    namespace
    {
        EPassExecutionMode classifyPassExecutionMode(const RGPassDescription* pass_desc) noexcept
        {
            if (pass_desc == nullptr)
                return EPassExecutionMode::RECORDER_FALLBACK;
            if (!pass_desc->hasKernel() || pass_desc->condition)
                return EPassExecutionMode::RECORDER_FALLBACK;

            // Prefer callback execution when a pass provides kernel_fn.
            if (pass_desc->kernel_fn)
                return EPassExecutionMode::COMPILED_CALLBACK;

            // CompiledNative iff the kernel has a registered emit function.
            const auto* desc = KernelRegistry::instance().find(pass_desc->kernel_id);
            if (desc && desc->emit)
                return EPassExecutionMode::COMPILED_NATIVE;

            return EPassExecutionMode::RECORDER_FALLBACK;
        }

        void classifyExecutionModes(RGCompiledGraph& compiled)
        {
            for (auto& cpass : compiled.compiled_passes)
                cpass.execution_mode = classifyPassExecutionMode(cpass.pass);
        }

        bool hasCompiledPasses(const RGCompiledGraph& compiled) noexcept
        {
            return std::any_of(compiled.compiled_passes.begin(), compiled.compiled_passes.end(),
                [](const RGCompiledPass& cpass)
                {
                    return cpass.execution_mode != EPassExecutionMode::RECORDER_FALLBACK;
                });
        }

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

        // Scan a write-list for the first resource that has a downstream reader.
        // Returns {consumer_pass_index, resource_index} or {MAX, MAX} if none found.
        // The consumer lookup is only for error diagnostics, so it runs lazily.
        struct ConsumedWriteResult { uint32_t consumer_pass; uint32_t resource; };

        ConsumedWriteResult findFirstConsumedWrite(
            const RGCompiledGraph& compiled,
            uint32_t                producer_pass_index,
            std::span<const uint32_t>    write_list,
            const std::vector<bool>&     resource_has_reader) noexcept
        {
            for (uint32_t ri : write_list)
            {
                if (ri >= resource_has_reader.size() || !resource_has_reader[ri])
                    continue;
                // Found a consumed resource — locate which pass reads it (for error message)
                for (uint32_t ci : compiled.execution_order)
                {
                    if (ci == producer_pass_index || ci >= compiled.compiled_passes.size())
                        continue;
                    const auto& c = compiled.compiled_passes[ci];
                    for (uint32_t cri : c.resources.read_images)
                        if (cri == ri) return {ci, ri};
                    for (uint32_t cri : c.resources.read_buffers)
                        if (cri == ri) return {ci, ri};
                }
                return {std::numeric_limits<uint32_t>::max(), ri};
            }
            return {std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()};
        }

        // ProgramEmitter is defined in ProgramEmitter.hpp (sinclude).

        // Emit a BeginRendering command with prebuilt header + per-attachment view patches.
        void emitBeginRendering(ProgramEmitter& e, uint32_t pi,
                                const RGCompiledPass& cpass,
                                const RGCompiledGraph& compiled)
        {
            const uint32_t group_idx = compiled.render_pass_layout.pass_to_group[pi];
            const auto& group = compiled.render_pass_layout.groups[group_idx];

            struct ViewPatch { uint32_t resource_index; uint8_t is_depth; uint8_t color_slot; uint8_t _pad[2]; };
            struct Header {
                uint32_t group_idx;
                uint32_t color_count;
                VkAttachmentLoadOp color_op;
                VkAttachmentLoadOp depth_op;
                VkFormat depth_format;
                uint8_t  patch_count;
                uint8_t  _pad[3];
            };

            lux::cxx::SmallVector<ViewPatch, 9> patches;
            uint32_t color_slot_idx = 0;
            for (const auto& tex_ref : cpass.pass->textures) {
                if (tex_ref.role == lux::common::ETextureRole::COLOR_ATTACHMENT
                    && color_slot_idx < group.key.color_count)
                {
                    ViewPatch vp{};
                    vp.resource_index = tex_ref.resource.index;
                    vp.is_depth = 0;
                    vp.color_slot = static_cast<uint8_t>(color_slot_idx++);
                    patches.push_back(vp);
                }
                else if (tex_ref.role == lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
                {
                    ViewPatch vp{};
                    vp.resource_index = tex_ref.resource.index;
                    vp.is_depth = 1;
                    vp.color_slot = 0;
                    patches.push_back(vp);
                }
            }

            const uint16_t total_size = static_cast<uint16_t>(
                sizeof(Header) + patches.size() * sizeof(ViewPatch));

            Header hdr{};
            hdr.group_idx    = group_idx;
            hdr.color_count  = group.key.color_count;
            hdr.color_op     = group.color_load_op;
            hdr.depth_op     = group.depth_load_op;
            hdr.depth_format = group.key.depth_stencil_format;
            hdr.patch_count  = static_cast<uint8_t>(patches.size());

            const uint32_t offset = static_cast<uint32_t>(e.program.command_data.size());
            {
                const auto* bytes = reinterpret_cast<const std::byte*>(&hdr);
                e.program.command_data.insert(e.program.command_data.end(), bytes, bytes + sizeof(Header));
            }
            if (!patches.empty()) {
                const auto* bytes = reinterpret_cast<const std::byte*>(patches.data());
                e.program.command_data.insert(e.program.command_data.end(),
                                              bytes, bytes + patches.size() * sizeof(ViewPatch));
            }
            e.program.commands.push_back({
                ExecutionProgram::Command::EType::BeginRendering, offset, total_size});
        }
        // Scan resources and build fast lookup tables for resize-tracking,
        // dynamic-import refresh and slot-based injection.
        void buildResourceLookupTables(RGCompiledGraph& compiled)
        {
            const auto& resources = compiled.original_graph.resources;
            const size_t res_count = resources.size();
            std::vector<bool> added_to_relative(res_count, false);

            for (size_t i = 0; i < res_count; ++i) {
                const auto& res = resources[i];
                const auto ri = static_cast<uint32_t>(i);

                // 1. Collect relative-sized textures (for recalculating sizes on Resize)
                if (res.type == ERGResourceType::TEXTURE) {
                    const auto& tex_desc = std::get<RGTextureDescription>(res.desc);
                    if (tex_desc.sizing_mode == ERGSizeMode::RELATIVE_MODE) {
                        compiled.relative_size_resources.push_back(ri);
                        added_to_relative[i] = true;
                    }
                }

                if (res.lifetime != ERGResourceLifetime::IMPORTED)
                    continue;

                // 2. Collect dynamic external resources (for precise handle refresh)
                bool has_dynamic_getter = false;
                if (res.import_info && res.import_info->image_getter != nullptr)
                    has_dynamic_getter = true;
                if (res.import_buffer_info && res.import_buffer_info->buffer_getter != nullptr)
                    has_dynamic_getter = true;
                if (has_dynamic_getter)
                    compiled.dynamic_external_resources.push_back(ri);

                // 3. Register slotted imported resources for fast per-frame injection
                if (!res.import_info || !res.import_info->slot.has_value())
                    continue;

                const size_t slot_idx = static_cast<size_t>(*res.import_info->slot);
                if (slot_idx >= kTargetSlotCount)
                    continue;

                // Slot-based resources also need resize tracking (avoid double-add
                // if already registered via RELATIVE_MODE above)
                if (!added_to_relative[i]) {
                    compiled.relative_size_resources.push_back(ri);
                    added_to_relative[i] = true;
                }

                // Map slot → logical resource index
                if (compiled.valid_resources[ri])
                    compiled.slot_resource_idx[slot_idx] = ri;
            }
        }

    } // namespace


    RGCompiledGraph RenderGraphCompiler::compile(RGGraphDescription graph, PipelineManager& pipeline_manager, const RGCompileOptions &options)
    {
        RGCompiledGraph compiled{};
        compiled.original_graph = std::move(graph);

        // 0) Resolve forward resource references — rewrite placeholder handles
        //    to actual resource indices.  Must run before everything else so that
        //    dependency analysis, render-pass planning, and all downstream phases
        //    see the resolved handles.
        resolveForwardReferences(compiled);

        // 0.5) Make bindResourceDS-declared external consumption visible to the
        //      graph: inject each declared_consume handle as a READ so dependency
        //      analysis (next), ordering and dead-pass elimination account for it.
        //      Must run AFTER forward-ref resolution (so declared handles are real)
        //      and BEFORE buildGlobalInfo (which runs the dependency analyzer).
        injectResourceDSDependencies(compiled);

        // 1) Global analysis: dependencies, lifetimes, resource validity, render pass layout
        if (!buildGlobalInfo(compiled, options))
            return compiled;

        // 1.5) Auto-fill transient-DS image layouts from each binding pass's resource
        //      role, so the descriptor layout always equals the layout computeBarriers
        //      transitions the image to (no hand-written, drift-prone layouts). Runs
        //      after forward-ref resolution + DS-write remap (resources are real) and
        //      before descriptor-set materialisation. (M3 layer 3)
        autoFillTransientDSLayouts(compiled);

        // 1.6) Warn about bindResourceDS bindings that don't declare the resource
        //      they consume — invisible to ordering / dead-pass elimination.
        //      Phase A: warn only (a later milestone makes this a hard error). (M3 layer 3)
        validateResourceDSBindings(compiled);

        // 2) Build compilation info for each pass (render pass / framebuffer / pipeline / resource mapping)
        if (!buildCompiledPasses(compiled, pipeline_manager))
            return compiled;

        // 3) Execution order = topological order
        computeExecutionOrder(compiled);

        // 3.1) Dead pass elimination (A-02): remove passes whose outputs contribute
        //      to no exported/imported resource.  Must run before barrier computation
        //      so that dead passes don't generate unnecessary barriers.
        eliminateDeadPasses(compiled);

        // 3.5) Assign target queue based on pass type
        computeQueueAssignment(compiled);

        // 3.6) Cross-queue dependencies: partition per-queue, compute timeline
        //      semaphore sync points (A-01). Also fail-fasts (sets compile_error) if
        //      the graph needs a queue-family ownership transfer we cannot emit (M4).
        computeCrossQueueDependencies(compiled);
        if (!compiled.compile_error.empty())
            return compiled; // M4: cross-queue EXCLUSIVE resource sharing needs unimplemented QFOT

        // 4) Based on execution order, compute Begin/End RenderPass for each graphics pass
        computeRenderPassBoundaries(compiled);

        // 4.5) Classify conditional passes as elective (intra-group additive)
        //      so the binding plan steps can conservatively reset after them.
        if (!classifyElectivePasses(compiled))
            return compiled;

        // 5) Based on execution order and render pass boundaries, compute minimum necessary BindPipeline timing
        computePipelineBindingPlan(compiled);

        // 5.1) Per-slot descriptor set layout-break mask (feeds runtime DS dedup)
        computeDescriptorBindingPlan(compiled);

        // 6) Compute Barriers
        computeBarriers(compiled);

        // (Split barriers REMOVED: the previous computeSplitBarriers emitted the
        // release/acquire halves as two plain vkCmdPipelineBarrier2 calls with
        // empty intersecting scopes (NONE/NONE) — per Vulkan's execution-
        // dependency-chain rule those never chain, silently dropping the
        // producer→consumer dependency. Real split barriers need VkEvent
        // (vkCmdSetEvent2/vkCmdWaitEvents2); until that exists the full
        // barrier stays on the consumer — correct and barely slower.)

        // 6.6) Build pre-built VkBarrier arrays (hot-path optimisation)
        buildPrebuiltBarriers(compiled);

        // 7) Compute Descriptor Sets
        computeDescriptorSets(compiled, pipeline_manager);

        // 8) Compute parallel recording groups
        computeParallelGroups(compiled);

        // 9) Pre-compute per-group attachment resource indices and static loadOps
        computeGroupLoadOps(compiled);

        // 10) Force full binding for passes in multi-CB parallel groups.
        //     Secondary command buffers do not inherit pipeline/DS state from
        //     each other, so each pass must independently bind everything.
        forceFullBindingForParallelPasses(compiled);

        // 10.5) Classify which passes can use compiled execution.
        classifyExecutionModes(compiled);

        // ===== Phase 1: kernel-driven fast-path compilation products =====
        if (hasCompiledPasses(compiled))
        {
            computeMeshBucketLayout(compiled, pipeline_manager);
            computeViewAllocatorPlan(compiled);
            buildBarrierProgram(compiled);
            buildQueueSubmitProgram(compiled);
            buildExecutionProgram(compiled);
        }

        compiled.valid = true;
        return compiled;
    }

    // ---------------------------
    // 1) Global Info
    // ---------------------------

    bool RenderGraphCompiler::buildGlobalInfo(RGCompiledGraph &compiled, const RGCompileOptions &options)
    {
        // Dependency analysis (topological order + resource lifetimes)
        compiled.dependency_info = DependencyAnalyzer::analyze(compiled.original_graph);
        if (compiled.dependency_info.has_cycle)
        {
            compiled.compile_error = "RenderGraph dependency cycle detected";
            return false;
        }

        // Compute per-resource validity (compiler no longer allocates GPU resources).
        // valid_resources[i] == true means the resource has a legal description and
        // can be allocated later by a per-view RGResourceState.
        const size_t res_count = compiled.original_graph.resources.size();
        compiled.valid_resources.resize(res_count, false);
        for (size_t i = 0; i < res_count; ++i) {
            const auto& res = compiled.original_graph.resources[i];
            if (res.lifetime == ERGResourceLifetime::IMPORTED) {
                bool valid_import = false;
                std::visit([&](auto&& desc) {
                    using T = std::decay_t<decltype(desc)>;
                    if constexpr (std::is_same_v<T, RGTextureDescription>)
                    {
                        if (res.import_info)
                            valid_import = static_cast<bool>(res.import_info->image_getter)
                                || res.import_info->slot.has_value();
                    }
                    else if constexpr (std::is_same_v<T, RGBufferDescription>)
                    {
                        valid_import = res.import_buffer_info
                            && static_cast<bool>(res.import_buffer_info->buffer_getter);
                    }
                }, res.desc);
                if (!valid_import)
                {
                    compiled.compile_error = "Invalid imported resource '" + res.name
                        + "': texture requires image_getter or slot; buffer requires buffer_getter.";
                    return false;
                }
                compiled.valid_resources[i] = true;
            } else if (res.lifetime == ERGResourceLifetime::FORWARD_REFERENCE) {
                // Unresolved forward reference: resolveForwardReferences() re-points
                // every reader of a *resolved* ref to the real resource index, so any
                // entry still tagged FORWARD_REFERENCE here is either orphaned (its
                // readers were re-mapped away) or genuinely unresolved (no pass writes
                // it). Either way it has no backing image/buffer, so marking it valid
                // would make computeBarriers emit a barrier against VK_NULL_HANDLE.
                // Mark invalid so dead-pass elimination prunes any reader. (M5)
                // M2: a REQUIRED reference left unresolved is a hard error (fail fast),
                // not a silent degrade to a null view / downstream VUID. Optional keeps
                // the prune behaviour (consumer degrades).
                if (res.reference_mode == ERGReference::Required) {
                    compiled.compile_error = "Required reference '" + res.name
                        + "' has no producer in this graph (a feature must create/import a "
                          "resource with this name — check feature registration / extension flags).";
                    return false;
                }
                compiled.valid_resources[i] = false;
            } else if (res.lifetime == ERGResourceLifetime::EXTERNAL) {
                // Feature-owned: no RG-backed image/buffer. Mark invalid so
                // computeBarriers skips it (the per-resource valid_resources guard
                // at the top of its texture/buffer loops means NO barrier is emitted
                // against a null handle). Dependency analysis + dead-pass elimination
                // read pass.textures/buffers directly (not valid_resources), so the
                // injected READ is still visible for ordering/reachability. (M3 layer 2)
                compiled.valid_resources[i] = false;
            } else {
                // TRANSIENT / PERSISTENT → valid if description exists
                compiled.valid_resources[i] = true;
            }
        }

        // RenderPass layout (render pass groups organized by key)
        compiled.render_pass_layout = RenderPassPlanner::plan(
            compiled.original_graph,
            compiled.dependency_info
        );
        if (!compiled.render_pass_layout.valid)
        {
            compiled.compile_error = compiled.render_pass_layout.error_message;
            return false;
        }

		// Scan resources, build fast lookup tables
		buildResourceLookupTables(compiled);

        return true;
    }

    // ---------------------------
    // 2) Build RGCompiledPass
    // ---------------------------

    bool RenderGraphCompiler::buildCompiledPasses(RGCompiledGraph &compiled, PipelineManager& pipeline_manager)
    {
        const uint32_t pass_count =
            static_cast<uint32_t>(compiled.original_graph.passes.size());

        compiled.compiled_passes.clear();
        compiled.compiled_passes.resize(pass_count);

        for (uint32_t pass_idx = 0; pass_idx < pass_count; ++pass_idx)
        {
            if (!buildSinglePass(compiled, pass_idx, pipeline_manager))
                return false;
        }
        return true;
    }

    bool RenderGraphCompiler::buildSinglePass(RGCompiledGraph &compiled, uint32_t pass_index, PipelineManager& pipeline_manager)
    {
        RGPassDescription &pass_desc = compiled.original_graph.passes[pass_index];
        RGCompiledPass    &cpass = compiled.compiled_passes[pass_index];

        cpass.pass = &pass_desc;
        cpass.pass_index = pass_index;

        // Propagate conditional execution pointers
        if (pass_desc.condition)
            cpass.condition = &pass_desc.condition;

        // Graphics pass: setup render pass / framebuffer / pipeline
        if (pass_desc.type == ERGPassType::GRAPHICS)
        {
            setupGraphicsPass(compiled, pass_index, cpass, pass_desc, pipeline_manager);
        }
        // DESIGN-01: Compute pass — populate layout/descriptor info from template
        else if (pass_desc.type == ERGPassType::COMPUTE
              || pass_desc.type == ERGPassType::ASYNC_COMPUTE)
        {
            if (!setupComputePass(compiled, cpass, pass_desc, pipeline_manager))
                return false;
        }

        // Resource mapping: map logical handles to physical handles
        mapPassTextures(compiled, cpass);
        mapPassBuffers(compiled, cpass);
        return true;
    }

    void RenderGraphCompiler::setupGraphicsPass(RGCompiledGraph &compiled, uint32_t pass_index, RGCompiledPass &cpass, RGPassDescription &pass_desc, PipelineManager& pipeline_manager)
    {
        const auto &layout = compiled.render_pass_layout;

        if (pass_index >= layout.pass_to_group.size())
            return;

        const uint32_t group_index = layout.pass_to_group[pass_index];
        if (group_index == std::numeric_limits<uint32_t>::max())
            return; // No layout, meaning this pass has no valid render pass configuration

        cpass.render.render_pass.index = group_index;
        cpass.render.framebuffer.index = group_index; // 1:1 for now, can be differentiated by swapchain image etc. in the future

        cpass.render.pipeline = VK_NULL_HANDLE;
        cpass.render.pipeline_variant_handles.clear();
        cpass.render.pipeline_variants.clear();
        cpass.render.pipeline_variant_layouts.clear();

        // Use the pipeline_template handle attached to the pass.
        // Some passes (e.g. ImGuiFeature) manage their own pipelines internally
        // and deliberately leave this handle at its sentinel value — skip them.
        if (pass_desc.pipeline_template.valid())
        {
            const auto pipeline_template = pass_desc.pipeline_template;

            const auto variantFeatureMask = [&](uint32_t variant_index) -> uint32_t
            {
                if (variant_index < pass_desc.pipeline_variant_features.size())
                    return pass_desc.pipeline_variant_features[variant_index];
                return 0u;
            };

            uint32_t subpass_index = 0;
            if (pass_index < layout.pass_to_subpass.size())
                subpass_index = layout.pass_to_subpass[pass_index];

            const auto &group = layout.groups[group_index];

            auto pipeline = pipeline_manager.getOrCreatePipeline(
                pipeline_template,
                group.key,
                subpass_index,
                variantFeatureMask(0u));

            cpass.render.pipeline = pipeline;
            cpass.render.subpass  = subpass_index;

            // C-4: Retain template handle + render pass key for per-item variant switching
            cpass.render.pipeline_template_handle = pipeline_template;
            cpass.render.render_pass_key = group.key;

            const auto& tmpl = pipeline_manager.getTemplate(pipeline_template);
            cpass.render.pipeline_layout = tmpl.pipeline_layout;
            cpass.render.descriptor_set_count = tmpl.descriptor_set_count;

            auto push_variant = [&](GraphicsPipelineHandle handle, VkPipeline vk_pipeline, VkPipelineLayout vk_layout)
            {
                if (!handle.valid() || vk_pipeline == VK_NULL_HANDLE || vk_layout == VK_NULL_HANDLE)
                    return;
                cpass.render.pipeline_variant_handles.push_back(handle);
                cpass.render.pipeline_variants.push_back(vk_pipeline);
                cpass.render.pipeline_variant_layouts.push_back(vk_layout);
            };

            // Variant index 0 is always the pass-level default pipeline.
            push_variant(pipeline_template, pipeline, tmpl.pipeline_layout);

            // Compile additional pipelines for multi-geometry passes
            uint32_t variant_index = 1u;
            for (auto extra_handle : pass_desc.additional_pipelines)
            {
                auto extra_pipeline = pipeline_manager.getOrCreatePipeline(
                    extra_handle, group.key, subpass_index, variantFeatureMask(variant_index));
                const auto& extra_tmpl = pipeline_manager.getTemplate(extra_handle);
                push_variant(extra_handle, extra_pipeline, extra_tmpl.pipeline_layout);

                ++variant_index;

                const auto geo_idx = static_cast<uint32_t>(extra_tmpl.geometry_type);
                if (geo_idx < PassRenderState::kGeometryTypeCount)
                {
                    cpass.render.geo_pipeline_table[geo_idx]   = extra_pipeline;
                    cpass.render.geo_layout_table[geo_idx]     = extra_tmpl.pipeline_layout;
                    cpass.render.geo_set_count_table[geo_idx]  = extra_tmpl.descriptor_set_count;
                }
            }

            // Fill the primary pipeline's geometry type entry
            const auto primary_geo = static_cast<uint32_t>(tmpl.geometry_type);
            if (primary_geo < PassRenderState::kGeometryTypeCount)
            {
                cpass.render.geo_pipeline_table[primary_geo]   = pipeline;
                cpass.render.geo_layout_table[primary_geo]     = tmpl.pipeline_layout;
                cpass.render.geo_set_count_table[primary_geo]  = tmpl.descriptor_set_count;
            }

            // Populate unfilled geometry type entries with the default pipeline
            // so that pipelineForGeometry() never falls back at runtime.
            for (uint32_t g = 0; g < PassRenderState::kGeometryTypeCount; ++g)
            {
                if (cpass.render.geo_pipeline_table[g] == VK_NULL_HANDLE)
                {
                    cpass.render.geo_pipeline_table[g]   = pipeline;
                    cpass.render.geo_layout_table[g]     = tmpl.pipeline_layout;
                    cpass.render.geo_set_count_table[g]  = tmpl.descriptor_set_count;
                }
            }
        }
    }

    // Compute pass setup. Legacy graphics-template fallback is removed:
    // compute/async-compute passes must declare compute_pipeline_handle.
    bool RenderGraphCompiler::setupComputePass(
        RGCompiledGraph& compiled,
        RGCompiledPass& cpass,
        RGPassDescription& pass_desc,
        PipelineManager& pipeline_manager)
    {
        cpass.render.bind_pipeline = true;  // Always bind compute pipelines
        cpass.render.pipeline = VK_NULL_HANDLE;

        if (!pass_desc.compute_pipeline_handle.valid())
        {
            compiled.compile_error = "RenderGraphCompiler: compute pass '" + pass_desc.name
                + "' is missing compute_pipeline_handle.";
            return false;
        }

        cpass.render.pipeline        = pipeline_manager.getComputePipeline(pass_desc.compute_pipeline_handle);
        cpass.render.pipeline_layout = pipeline_manager.getComputeLayout(pass_desc.compute_pipeline_handle);
        if (cpass.render.pipeline == VK_NULL_HANDLE || cpass.render.pipeline_layout == VK_NULL_HANDLE)
        {
            compiled.compile_error = "RenderGraphCompiler: compute pass '" + pass_desc.name
                + "' references an invalid compute pipeline handle.";
            return false;
        }

        return true;
    }

    void RenderGraphCompiler::mapPassTextures(const RGCompiledGraph &compiled, RGCompiledPass &cpass)
    {
        const auto &pass = *cpass.pass;

        for (const auto &tex_ref : pass.textures)
        {
            const uint32_t res_idx = tex_ref.resource.index;
            if (res_idx >= compiled.valid_resources.size() || !compiled.valid_resources[res_idx])
                continue;

            const bool is_read =
                tex_ref.usage == ERGResourceUsage::READ ||
                tex_ref.usage == ERGResourceUsage::READ_WRITE;

            const bool is_write =
                tex_ref.usage == ERGResourceUsage::WRITE ||
                tex_ref.usage == ERGResourceUsage::READ_WRITE;

            if (is_read)
                cpass.resources.read_images.push_back(res_idx);
            if (is_write)
                cpass.resources.write_images.push_back(res_idx);
        }
    }

    void RenderGraphCompiler::mapPassBuffers(const RGCompiledGraph &compiled, RGCompiledPass &cpass)
    {
        const auto &pass = *cpass.pass;

        for (const auto &buf_ref : pass.buffers)
        {
            const uint32_t res_idx = buf_ref.resource.index;
            if (res_idx >= compiled.valid_resources.size() || !compiled.valid_resources[res_idx])
                continue;

            const bool is_read =
                buf_ref.usage == ERGResourceUsage::READ ||
                buf_ref.usage == ERGResourceUsage::READ_WRITE;

            const bool is_write =
                buf_ref.usage == ERGResourceUsage::WRITE ||
                buf_ref.usage == ERGResourceUsage::READ_WRITE;

            if (is_read)
                cpass.resources.read_buffers.push_back(res_idx);
            if (is_write)
                cpass.resources.write_buffers.push_back(res_idx);
        }
    }

    // ---------------------------
    // 3) Execution Order
    // ---------------------------

    void RenderGraphCompiler::computeExecutionOrder(RGCompiledGraph &compiled)
    {
        compiled.execution_order = compiled.dependency_info.pass_topological_order;
    }

    // ---------------------------
    // ---------------------------
    // ---------------------------
    // 0) Resolve Forward References
    // ---------------------------
    //
    // Forward references (ERGResourceLifetime::FORWARD_REFERENCE) are placeholder
    // resources created by RGBuilder::referenceTexture/Buffer().  They allow features
    // to declare reads on resources that haven't been created yet (e.g. GBuffer textures
    // referenced before DeferredGBufferFeature runs).
    //
    // This pass rewrites every occurrence of a forward-ref handle in all pass texture/
    // buffer declarations to the actual TRANSIENT/IMPORTED resource with the same name.
    // Unresolved forward references are logged and their dependent passes will be pruned
    // by the subsequent dead-pass elimination step.

    void RenderGraphCompiler::resolveForwardReferences(RGCompiledGraph& compiled)
    {
        auto& graph = compiled.original_graph;
        const uint32_t resource_count = static_cast<uint32_t>(graph.resources.size());

        // Step 1: Build name → actual resource index map (non-forward-ref resources only)
        std::unordered_map<std::string_view, uint32_t> actual_resources;
        std::vector<uint32_t> forward_refs;

        for (uint32_t i = 0; i < resource_count; ++i)
        {
            if (graph.resources[i].lifetime == ERGResourceLifetime::FORWARD_REFERENCE)
                forward_refs.push_back(i);
            else
                actual_resources[graph.resources[i].name] = i;
        }

        if (forward_refs.empty()) return;

        // Step 2: Build forward-ref index → actual index mapping
        std::vector<uint32_t> remap(resource_count);
        for (uint32_t i = 0; i < resource_count; ++i)
            remap[i] = i; // identity by default

        for (uint32_t fwd_idx : forward_refs)
        {
            const auto& fwd = graph.resources[fwd_idx];
            auto it = actual_resources.find(fwd.name);
            if (it != actual_resources.end())
            {
                remap[fwd_idx] = it->second;
            }
            else
            {
                // Unresolved forward reference — no pass writes to this resource,
                // so dead-pass elimination will prune any pass that reads it.
            }
        }

        // Step 3: Rewrite all pass texture/buffer references
        for (auto& pass : graph.passes)
        {
            for (auto& tex : pass.textures)
            {
                if (remap[tex.resource.index] != tex.resource.index)
                    tex.resource.index = remap[tex.resource.index];
            }
            for (auto& buf : pass.buffers)
            {
                if (remap[buf.resource.index] != buf.resource.index)
                    buf.resource.index = remap[buf.resource.index];
            }
        }

        // Transient-DS writes also reference resources by handle (e.g. a DS built
        // from builder.referenceTexture("SceneDepth")). Without this remap a write
        // could keep pointing at the unresolved forward-ref index while the pass's
        // own .read() got remapped to the real one — so autoFillTransientDSLayouts
        // (and the recorder's view lookup) would miss the match. (M3 layer 3)
        for (auto& tds : graph.transient_descriptor_sets)
        {
            for (auto& w : tds.writes)
            {
                if (w.resource.index < resource_count
                    && remap[w.resource.index] != w.resource.index)
                    w.resource.index = remap[w.resource.index];
            }
        }
    }

    // 0.5) Inject bindResourceDS external consumption as READ dependencies
    // ---------------------------
    void RenderGraphCompiler::injectResourceDSDependencies(RGCompiledGraph& compiled)
    {
        auto& graph = compiled.original_graph;
        const uint32_t resource_count = static_cast<uint32_t>(graph.resources.size());

        for (auto& pass : graph.passes)
        {
            for (const auto& bind : pass.ds_bindings)
            {
                if (bind.source != EDSBindingSource::Resource)
                    continue;
                const uint32_t ri = bind.declared_consume.index;
                if (ri >= resource_count)
                    continue;   // not declared (invalid handle) — relies on markSideEffect

                if (bind.consume_type == ERGResourceType::TEXTURE)
                {
                    bool exists = false;
                    for (const auto& t : pass.textures)
                        if (t.resource.index == ri) { exists = true; break; }
                    if (exists) continue;   // already declared via .read()/.write()

                    RGPassTextureRef ref{};
                    ref.resource = bind.declared_consume;
                    ref.role     = bind.consume_tex_role;
                    ref.usage    = ERGResourceUsage::READ;
                    pass.textures.push_back(ref);
                }
                else
                {
                    bool exists = false;
                    for (const auto& b : pass.buffers)
                        if (b.resource.index == ri) { exists = true; break; }
                    if (exists) continue;

                    RGPassBufferRef ref{};
                    ref.resource = bind.declared_consume;
                    ref.usage    = ERGResourceUsage::READ;
                    ref.role     = ERGBufferRole::STORAGE;
                    pass.buffers.push_back(ref);
                }
            }
        }
    }

    // 1.5) Auto-fill transient-DS image layouts from the binding pass's role
    // ---------------------------
    void RenderGraphCompiler::autoFillTransientDSLayouts(RGCompiledGraph& compiled)
    {
        auto& graph = compiled.original_graph;
        const uint32_t tds_count = static_cast<uint32_t>(graph.transient_descriptor_sets.size());
        if (tds_count == 0) return;
        const uint32_t resource_count = static_cast<uint32_t>(graph.resources.size());

        // Map each transient DS to the first pass that binds it (in practice a DS
        // is built for and bound by exactly one pass).
        constexpr uint32_t kNoPass = ~0u;
        std::vector<uint32_t> tds_to_pass(tds_count, kNoPass);
        for (uint32_t pi = 0; pi < graph.passes.size(); ++pi)
        {
            for (const auto& bind : graph.passes[pi].ds_bindings)
            {
                if (bind.source == EDSBindingSource::Transient
                    && bind.transient_ds_index < tds_count
                    && tds_to_pass[bind.transient_ds_index] == kNoPass)
                    tds_to_pass[bind.transient_ds_index] = pi;
            }
        }

        for (uint32_t ti = 0; ti < tds_count; ++ti)
        {
            const uint32_t pi = tds_to_pass[ti];
            if (pi == kNoPass) continue;   // unbound DS — nothing to derive a layout from
            const RGPassDescription& pass = graph.passes[pi];
            auto& tds = graph.transient_descriptor_sets[ti];

            for (auto& w : tds.writes)
            {
                switch (w.descriptor_type)
                {
                case EDescriptorType::SAMPLED_IMAGE:
                case EDescriptorType::STORAGE_IMAGE:
                case EDescriptorType::COMBINED_IMAGE_SAMPLER:
                case EDescriptorType::INPUT_ATTACHMENT:
                    break;
                default:
                    continue;   // buffers / non-image descriptors carry no layout
                }

                const uint32_t ri = w.resource.index;
                if (ri < resource_count)
                {
                    if (const auto* tex_desc = std::get_if<RGTextureDescription>(&graph.resources[ri].desc))
                    {
                        // The binding pass's role for this resource (covers injected
                        // reads). Without a .read()/.write() to derive from, keep the
                        // author's value.
                        const RGPassTextureRef* ref = nullptr;
                        for (const auto& t : pass.textures)
                            if (t.resource.index == ri) { ref = &t; break; }
                        if (ref != nullptr)
                            w.image_layout = convertVkImageLayout(determineTextureState(*ref, *tex_desc, pass.type).layout);
                    }
                }

                // F2: descriptor type ↔ image layout consistency. The layout is
                // derived from the pass role, so a mismatch means the feature declared
                // a role inconsistent with the descriptor type (STORAGE_IMAGE needs
                // GENERAL; sampled/combined/input needs a READ_ONLY layout). (M3 layer 3)
                const bool layout_ok =
                    (w.descriptor_type == EDescriptorType::STORAGE_IMAGE)
                        ? (w.image_layout == EImageLayout::GENERAL)
                        : (w.image_layout == EImageLayout::SHADER_READ_ONLY_OPTIMAL ||
                           w.image_layout == EImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL);
                if (!layout_ok)
                    std::fprintf(stderr,
                        "[RG] transient DS '%s': descriptor type %d resolved to layout %d, which is "
                        "inconsistent (STORAGE_IMAGE needs GENERAL; sampled/input needs READ_ONLY) — "
                        "check the role declared for this resource.\n",
                        tds.name.c_str(), static_cast<int>(w.descriptor_type),
                        static_cast<int>(w.image_layout));
            }
        }
    }

    // 1.6) Validate that every bindResourceDS declares the resource it consumes
    // ---------------------------
    void RenderGraphCompiler::validateResourceDSBindings(const RGCompiledGraph& compiled)
    {
        const auto& graph = compiled.original_graph;
        const uint32_t rc = static_cast<uint32_t>(graph.resources.size());

        for (const auto& pass : graph.passes)
        {
            if (pass.has_side_effect)
                continue;   // explicit opt-out (legacy markSideEffect path)

            for (const auto& bind : pass.ds_bindings)
            {
                if (bind.source != EDSBindingSource::Resource)
                    continue;
                if (bind.declared_consume.index < rc)
                    continue;   // declared — the graph sees the consumption

                // Phase A (M3): warn only. The dependency is invisible to ordering /
                // dead-pass elimination; the pass survives only by implicit liveness
                // (it writes some other exported resource) or markSideEffect. A later
                // milestone promotes this to a hard compile error. (M3 layer 3)
                std::fprintf(stderr,
                    "[RG] pass '%s' slot %u: bindResourceDS without declared_consume — the graph "
                    "cannot see this external read for ordering/dead-pass. Pass it the RG handle, "
                    "or markSideEffect() to opt out.\n",
                    pass.name.c_str(), bind.slot);
            }
        }
    }

    // 3.1) Dead Pass Elimination (A-02)
    // ---------------------------
    //
    // Removes passes whose outputs are never consumed by an "exported" resource.
    // Exported resources are:
    //   - IMPORTED resources with final_layout != UNDEFINED (e.g. backbuffer → PRESENT_SRC)
    //   - IMPORTED resources with an import_buffer_info (buffers read back by the CPU)
    //   - PERSISTENT resources that survive across frames
    //
    // Algorithm: BFS backward from exported resources through the producer/consumer
    // graph.  Any pass NOT reachable is considered dead and removed from
    // execution_order (+ disabled so render-pass group iteration skips it).

    void RenderGraphCompiler::eliminateDeadPasses(RGCompiledGraph& compiled)
    {
        const auto& graph = compiled.original_graph;
        const uint32_t pass_count     = static_cast<uint32_t>(graph.passes.size());
        const uint32_t resource_count = static_cast<uint32_t>(graph.resources.size());

        if (pass_count == 0) return;

        // -------------------------------------------------------------------
        // Step 1: Identify "exported" resources that must be produced.
        // -------------------------------------------------------------------
        std::vector<bool> resource_is_exported(resource_count, false);
        for (uint32_t i = 0; i < resource_count; ++i)
        {
            const auto& res = graph.resources[i];

            if (res.lifetime == ERGResourceLifetime::PERSISTENT)
            {
                resource_is_exported[i] = true;
            }
            else if (res.lifetime == ERGResourceLifetime::EXTERNAL)
            {
                // Feature-owned, consumed/produced outside the graph: a pass that
                // WRITES one must never be pruned (mirrors the old markSideEffect
                // seed). Pure readers (Light/Material) have no writer, so this is a
                // no-op for them. (M3 layer 2)
                resource_is_exported[i] = true;
            }
            else if (res.lifetime == ERGResourceLifetime::IMPORTED)
            {
                // Imported texture with a required final layout (e.g. swapchain → PRESENT_SRC)
                if (res.import_info && res.import_info->final_layout != VK_IMAGE_LAYOUT_UNDEFINED)
                    resource_is_exported[i] = true;
                // Imported buffer (assumed needed by CPU / external system)
                if (res.import_buffer_info)
                    resource_is_exported[i] = true;
            }
        }

        // -------------------------------------------------------------------
        // Step 2: Build resource→writers and pass→reads mappings.
        // -------------------------------------------------------------------
        std::vector<std::vector<uint32_t>> resource_writers(resource_count);
        std::vector<std::vector<uint32_t>> pass_reads(pass_count);

        for (uint32_t pi = 0; pi < pass_count; ++pi)
        {
            const auto& pass = graph.passes[pi];
            for (const auto& tex : pass.textures)
            {
                if (tex.usage == ERGResourceUsage::WRITE || tex.usage == ERGResourceUsage::READ_WRITE)
                    resource_writers[tex.resource.index].push_back(pi);
                if (tex.usage == ERGResourceUsage::READ  || tex.usage == ERGResourceUsage::READ_WRITE)
                    pass_reads[pi].push_back(tex.resource.index);
            }
            for (const auto& buf : pass.buffers)
            {
                if (buf.usage == ERGResourceUsage::WRITE || buf.usage == ERGResourceUsage::READ_WRITE)
                    resource_writers[buf.resource.index].push_back(pi);
                if (buf.usage == ERGResourceUsage::READ  || buf.usage == ERGResourceUsage::READ_WRITE)
                    pass_reads[pi].push_back(buf.resource.index);
            }
        }

        // Ping-pong closure (M1): the PREVIOUS handle and the CURRENT handle are the
        // same logical resource. A reader of previous() must keep the producer of
        // current() alive, so give the prev resource the same writers as its peer —
        // otherwise the BFS would prune the build pass (e.g. HZB) and the pyramid
        // would never be produced (the old over-cull root cause, now structural).
        for (uint32_t i = 0; i < resource_count; ++i)
        {
            const auto& res = graph.resources[i];
            if (res.lifetime == ERGResourceLifetime::PING_PONG
                && res.ring_phase > 0 && res.pingpong_peer >= 0)
            {
                const auto peer = static_cast<uint32_t>(res.pingpong_peer);
                if (peer < resource_count)
                    for (uint32_t w : resource_writers[peer])
                        resource_writers[i].push_back(w);
            }
        }

        // -------------------------------------------------------------------
        // Step 3: BFS backward from exported resources.
        //         Seed: every pass that writes to an exported resource.
        //         Expand: for each alive pass, all writers of its inputs are alive.
        // -------------------------------------------------------------------
        std::vector<bool> pass_alive(pass_count, false);

        // Worklist (vector used as a stack — avoids <queue> include)
        std::vector<uint32_t> worklist;
        worklist.reserve(pass_count);

        for (uint32_t ri = 0; ri < resource_count; ++ri)
        {
            if (!resource_is_exported[ri]) continue;
            for (uint32_t writer_pass : resource_writers[ri])
            {
                if (!pass_alive[writer_pass])
                {
                    pass_alive[writer_pass] = true;
                    worklist.push_back(writer_pass);
                }
            }
        }

        // Seed: passes that write a FEATURE-OWNED resource consumed outside the
        // RenderGraph (e.g. HzbBuild → the cull samples the HZB via a feature DS,
        // not an RG .read). The graph cannot see their consumer, so without this
        // they would be pruned and their output silently never produced.
        for (uint32_t pi = 0; pi < pass_count; ++pi)
        {
            if (graph.passes[pi].has_side_effect && !pass_alive[pi])
            {
                pass_alive[pi] = true;
                worklist.push_back(pi);
            }
        }

        while (!worklist.empty())
        {
            const uint32_t pi = worklist.back();
            worklist.pop_back();

            for (uint32_t ri : pass_reads[pi])
            {
                for (uint32_t writer : resource_writers[ri])
                {
                    if (!pass_alive[writer])
                    {
                        pass_alive[writer] = true;
                        worklist.push_back(writer);
                    }
                }
            }
        }

        // -------------------------------------------------------------------
        // Step 4: Filter execution_order and disable dead passes.
        // -------------------------------------------------------------------
        const size_t original_count = compiled.execution_order.size();

        std::vector<uint32_t> new_order;
        new_order.reserve(original_count);
        for (uint32_t pi : compiled.execution_order)
        {
            if (pass_alive[pi])
            {
                new_order.push_back(pi);
            }
            else
            {
                compiled.original_graph.passes[pi].enabled = false;
            }
        }

        compiled.execution_order = std::move(new_order);

        // Also filter dead passes from render-pass groups so the recorder
        // never encounters a dead subpass during group iteration.
        for (auto& group : compiled.render_pass_layout.groups)
        {
            std::erase_if(group.passes, [&](const RGPassInRenderPass& p) {
                return !pass_alive[p.pass_index];
            });
        }
    }

    // ---------------------------
    // 3.5) Queue Assignment
    // ---------------------------

    void RenderGraphCompiler::computeQueueAssignment(RGCompiledGraph& compiled)
    {
        for (auto& cpass : compiled.compiled_passes)
        {
            if (!cpass.pass) continue;

            switch (cpass.pass->type)
            {
            case ERGPassType::GRAPHICS:
                cpass.queue_type = ERGQueueType::GRAPHICS;
                break;
            case ERGPassType::COMPUTE:
                // Regular compute runs on graphics queue (shares command buffer)
                cpass.queue_type = ERGQueueType::GRAPHICS;
                break;
            case ERGPassType::ASYNC_COMPUTE:
                cpass.queue_type = ERGQueueType::COMPUTE;
                break;
            case ERGPassType::TRANSFER:
                // Regular transfer runs on graphics queue
                cpass.queue_type = ERGQueueType::GRAPHICS;
                break;
            case ERGPassType::ASYNC_TRANSFER:
                cpass.queue_type = ERGQueueType::TRANSFER;
                break;
            }
        }

    }

    // ---------------------------
    // 3.6) Cross-Queue Dependencies (A-01)
    // ---------------------------
    //
    // When the graph contains ASYNC_COMPUTE or ASYNC_TRANSFER passes, the
    // compiler:
    //   1. Partitions execution_order into per-queue sub-orders.
    //   2. Walks the resource producer/consumer chain to find cross-queue
    //      dependencies.
    //   3. Assigns monotonically increasing timeline-semaphore values at
    //      each cross-queue sync point.
    //   4. Records the dependencies on the corresponding RGCompiledPass
    //      (wait_dependencies / signal_dependencies).
    //
    // Step 5 — queue-family ownership transfer (QFOT) for resources that cross
    // queue boundaries — is NOT implemented. RG resources are EXCLUSIVE and the
    // recorder always emits VK_QUEUE_FAMILY_IGNORED, so a real cross-queue
    // resource dependency cannot be made correct yet. Instead of silently
    // corrupting data, the cross-queue walk below fail-fasts (sets compile_error)
    // the moment it sees such a dependency (review item M4); compile() then aborts.
    //
    // At record time the RGVulkanRecorder creates a single VkSemaphore
    // (timeline type) and uses these values for cross-queue synchronization.

    // H: Derive precise VkPipelineStageFlags2 from resource role + pass type,
    //    replacing the former blanket ALL_COMMANDS_BIT on semaphore sync points.
    static VkPipelineStageFlags2 writeStageForTexture(lux::common::ETextureRole role, ERGPassType pt)
    {
        switch (role) {
        case lux::common::ETextureRole::COLOR_ATTACHMENT:         return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        case lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT: return VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        case lux::common::ETextureRole::UNORDERED_ACCESS:         return shaderStageForPass(pt);
        default: return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
    }

    static VkPipelineStageFlags2 readStageForTexture(lux::common::ETextureRole role, ERGPassType pt)
    {
        switch (role) {
        case lux::common::ETextureRole::COLOR_ATTACHMENT:         return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        case lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT: return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        case lux::common::ETextureRole::SAMPLED:
        case lux::common::ETextureRole::UNORDERED_ACCESS:         return shaderStageForPass(pt);
        default: return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
    }

    static VkPipelineStageFlags2 writeStageForBuffer(ERGBufferRole role, ERGPassType pt)
    {
        if (pt == ERGPassType::TRANSFER || pt == ERGPassType::ASYNC_TRANSFER)
            return VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        switch (role) {
        case ERGBufferRole::STORAGE: return shaderStageForPass(pt);
        default: return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
    }

    static VkPipelineStageFlags2 readStageForBuffer(ERGBufferRole role, ERGPassType pt)
    {
        if (pt == ERGPassType::TRANSFER || pt == ERGPassType::ASYNC_TRANSFER)
            return VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        switch (role) {
        case ERGBufferRole::VERTEX:   return VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        case ERGBufferRole::INDEX:    return VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        case ERGBufferRole::INDIRECT: return VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        case ERGBufferRole::CONSTANT:
            // Constant/uniform reads span all graphics stages on a graphics pass
            // (not just FRAGMENT) — so this keeps its own ternary, not shaderStageForPass.
            return (pt == ERGPassType::COMPUTE || pt == ERGPassType::ASYNC_COMPUTE)
                ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                : VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        case ERGBufferRole::STORAGE: return shaderStageForPass(pt);
        default: return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
    }

    static const char* queueTypeName(ERGQueueType q)
    {
        switch (q) {
        case ERGQueueType::GRAPHICS: return "GRAPHICS";
        case ERGQueueType::COMPUTE:  return "COMPUTE";
        case ERGQueueType::TRANSFER: return "TRANSFER";
        }
        return "UNKNOWN";
    }

    void RenderGraphCompiler::computeCrossQueueDependencies(RGCompiledGraph& compiled)
    {
        auto& mq = compiled.multi_queue_info;

        // 1) Partition execution_order into per-queue lists
        for (uint32_t pi : compiled.execution_order)
        {
            const auto& cpass = compiled.compiled_passes[pi];
            switch (cpass.queue_type)
            {
            case ERGQueueType::GRAPHICS: mq.graphics_order.push_back(pi); break;
            case ERGQueueType::COMPUTE:  mq.compute_order.push_back(pi);  break;
            case ERGQueueType::TRANSFER: mq.transfer_order.push_back(pi); break;
            }
        }

        mq.has_async_work = !mq.compute_order.empty() || !mq.transfer_order.empty();
        if (!mq.has_async_work) return;

        // 2) Walk execution order and detect cross-queue dependencies
        //    For each pass that reads a resource whose last writer was on a
        //    *different* queue, we create a timeline-semaphore sync point.
        struct WriterInfo
        {
            uint32_t              pass_index   = UINT32_MAX;
            ERGQueueType          queue        = ERGQueueType::GRAPHICS;
            VkPipelineStageFlags2 write_stage  = 0; // H: accumulated write stage mask
        };

        const uint32_t resource_count =
            static_cast<uint32_t>(compiled.original_graph.resources.size());
        std::vector<WriterInfo> last_writer(resource_count);

        uint64_t semaphore_counter = 0;

        for (uint32_t pi : compiled.execution_order)
        {
            auto& cpass       = compiled.compiled_passes[pi];
            const auto* desc  = cpass.pass;
            if (!desc) continue;

            // --- Check reads for cross-queue producer dependency ---
            // H: consumer_stage parameter enables precise semaphore wait masks.
            auto check_cross_queue_read = [&](uint32_t res_idx,
                                              VkPipelineStageFlags2 consumer_stage)
            {
                if (!compiled.compile_error.empty()) return; // M4: already rejected — stop analysis
                if (res_idx >= resource_count) return;
                const auto& writer = last_writer[res_idx];
                if (writer.pass_index == UINT32_MAX) return;
                if (writer.queue == cpass.queue_type) return; // same queue — no sync needed

                // M4 fail-fast guard — genuine cross-queue resource dependency.
                // A resource written on one queue and read on another needs a queue-
                // family ownership transfer (QFOT): RG resources are created
                // VK_SHARING_MODE_EXCLUSIVE and the recorder emits every barrier with
                // VK_QUEUE_FAMILY_IGNORED, so without an explicit release-on-producer /
                // acquire-on-consumer ownership handoff the consumer reads UNDEFINED
                // contents. QFOT is not implemented, so rather than silently corrupt
                // data we reject the graph here. The timeline-semaphore sync-point code
                // below is the (currently unreachable) future home for real multi-queue
                // support; enabling it means implementing QFOT and removing this guard.
                {
                    const auto& wname = compiled.original_graph.resources[res_idx].name;
                    compiled.compile_error =
                        std::string("RenderGraph needs a cross-queue ownership transfer for resource '")
                        + wname + "' (written on " + queueTypeName(writer.queue)
                        + ", read on " + queueTypeName(cpass.queue_type)
                        + "), but queue-family ownership transfer (QFOT) is not implemented. "
                          "Implement QFOT before using ASYNC_COMPUTE / ASYNC_TRANSFER passes that "
                          "share EXCLUSIVE resources across queues (see render review item M4).";
                    return;
                }

                const VkPipelineStageFlags2 producer_stage =
                    writer.write_stage ? writer.write_stage : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

                // Cross-queue: writer.queue → cpass.queue_type
                // Avoid duplicate sync points if the same (producer, consumer) pair
                // was already linked for another resource.
                auto& producer = compiled.compiled_passes[writer.pass_index];

                // Check for an existing signal from this producer to this consumer
                for (auto& wd : cpass.wait_dependencies)
                {
                    if (wd.src_pass_index == writer.pass_index)
                    {
                        // H: Widen stage masks for the existing sync point
                        wd.wait_stage   |= consumer_stage;
                        wd.signal_stage |= producer_stage;
                        for (auto& sd : producer.signal_dependencies) {
                            if (sd.signal_value == wd.signal_value) {
                                sd.signal_stage |= producer_stage;
                                break;
                            }
                        }
                        return;
                    }
                }

                // New timeline semaphore sync point
                ++semaphore_counter;

                RGCompiledPass::QueueDependency dep{};
                dep.src_pass_index = writer.pass_index;
                dep.semaphore      = VK_NULL_HANDLE; // assigned at record time
                dep.signal_value   = semaphore_counter;
                dep.signal_stage   = producer_stage;
                dep.wait_stage     = consumer_stage;

                producer.signal_dependencies.push_back(dep);
                cpass.wait_dependencies.push_back(dep);

                // NOTE: queue-family ownership transfer (QFOT) is NOT implemented.
                // RGCompiledPass::sync.{acquire,release}_ownership_barriers and
                // src/dst_queue_family are reserved for it but never populated, and
                // computeBarriers()/buildPrebuiltBarriers() emit VK_QUEUE_FAMILY_IGNORED
                // unconditionally. The M4 guard above aborts compilation before any
                // cross-queue resource dependency reaches this point, so this sync-point
                // code is currently unreachable; it remains as the implementation site
                // for true multi-queue support. To enable it: implement QFOT (recorder
                // overrides of src/dstQueueFamilyIndex + release/acquire submit handoff),
                // then remove the M4 guard.
            };

            for (const auto& tex : desc->textures)
            {
                if (tex.usage == ERGResourceUsage::READ || tex.usage == ERGResourceUsage::READ_WRITE)
                    check_cross_queue_read(tex.resource.index,
                                           readStageForTexture(tex.role, desc->type));
            }
            for (const auto& buf : desc->buffers)
            {
                if (buf.usage == ERGResourceUsage::READ || buf.usage == ERGResourceUsage::READ_WRITE)
                    check_cross_queue_read(buf.resource.index,
                                           readStageForBuffer(buf.role, desc->type));
            }

            // --- Update last_writer for writes (H: track write stage) ---
            for (const auto& tex : desc->textures)
            {
                if (tex.usage == ERGResourceUsage::WRITE || tex.usage == ERGResourceUsage::READ_WRITE) {
                    auto& w = last_writer[tex.resource.index];
                    const auto ws = writeStageForTexture(tex.role, desc->type);
                    if (w.pass_index == pi)
                        w.write_stage |= ws; // same pass, accumulate
                    else
                        w = { pi, cpass.queue_type, ws };
                }
            }
            for (const auto& buf : desc->buffers)
            {
                if (buf.usage == ERGResourceUsage::WRITE || buf.usage == ERGResourceUsage::READ_WRITE) {
                    auto& w = last_writer[buf.resource.index];
                    const auto ws = writeStageForBuffer(buf.role, desc->type);
                    if (w.pass_index == pi)
                        w.write_stage |= ws;
                    else
                        w = { pi, cpass.queue_type, ws };
                }
            }
        }
    }

    // ---------------------------
    // 4) RenderPass Begin/End
    // ---------------------------

    void RenderGraphCompiler::computeRenderPassBoundaries(RGCompiledGraph &compiled)
    {
        const auto &order = compiled.execution_order;
        const uint32_t count = static_cast<uint32_t>(order.size());

        if (count == 0)
            return;

        for (auto &cpass : compiled.compiled_passes)
        {
            cpass.render.begin_render_pass = false;
            cpass.render.end_render_pass   = false;
        }

        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t curr_idx = order[i];
            RGCompiledPass &curr = compiled.compiled_passes[curr_idx];

            if (curr.pass->type != ERGPassType::GRAPHICS)
                continue;

            const uint32_t curr_rp = curr.render.render_pass.index;

            // ------ Should we BeginRenderPass? ------
            bool begin = true;
            if (i > 0)
            {
                const uint32_t prev_idx = order[i - 1];
                const RGCompiledPass &prev = compiled.compiled_passes[prev_idx];

                if (prev.pass->type == ERGPassType::GRAPHICS &&
                    prev.render.render_pass.index == curr_rp)
                {
                    begin = false;
                }
            }

            // ------ Should we EndRenderPass? ------
            bool end = true;
            if (i + 1 < count)
            {
                const uint32_t next_idx = order[i + 1];
                const RGCompiledPass &next = compiled.compiled_passes[next_idx];

                if (next.pass->type == ERGPassType::GRAPHICS &&
                    next.render.render_pass.index == curr_rp)
                {
                    end = false;
                }
            }

            curr.render.begin_render_pass = begin;
            curr.render.end_render_pass = end;
        }
    }

    // ---------------------------
    // 5) Pipeline Binding Strategy
    // ---------------------------

    void RenderGraphCompiler::computePipelineBindingPlan(RGCompiledGraph &compiled)
    {
        const auto &order = compiled.execution_order;
        const uint32_t count = static_cast<uint32_t>(order.size());

        if (count == 0)
            return;

        VkPipeline last_bound_pipeline = VK_NULL_HANDLE;

        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t idx = order[i];
            RGCompiledPass &curr = compiled.compiled_passes[idx];

            curr.render.bind_pipeline = false;

            // Entering a new render pass: reset pipeline state
            if (curr.render.begin_render_pass)
            {
                last_bound_pipeline = VK_NULL_HANDLE;
            }

            // DESIGN-01: Also handle COMPUTE / ASYNC_COMPUTE passes.
            // Compute passes are outside render passes; graphics and compute bind
            // points are independent, so a compute pass always binds its pipeline.
            if ((curr.pass->type == ERGPassType::COMPUTE || curr.pass->type == ERGPassType::ASYNC_COMPUTE)
                && curr.render.pipeline != VK_NULL_HANDLE)
            {
                curr.render.bind_pipeline = true;
                // Don't update last_bound_pipeline — compute/graphics are independent
            }
            else if (curr.pass->type == ERGPassType::GRAPHICS && curr.render.pipeline != VK_NULL_HANDLE)
            {
                if (last_bound_pipeline != curr.render.pipeline)
                {
                    curr.render.bind_pipeline = true;
                    last_bound_pipeline = curr.render.pipeline;
                }
            }
            else
            {
                curr.render.bind_pipeline = false;
            }

            // Leaving render pass: consider pipeline state invalidated
            if (curr.render.end_render_pass)
            {
                last_bound_pipeline = VK_NULL_HANDLE;
            }

            // After an elective (conditional) pass the runtime pipeline state is
            // unpredictable — the pass may or may not have executed.  Reset
            // tracking so the next pass conservatively re-binds.
            if (curr.elective_kind != EElectiveKind::NONE)
                last_bound_pipeline = VK_NULL_HANDLE;
        }
    }

    // ---------------------------
    // 5.1) Descriptor Binding Plan
    // ---------------------------

    void RenderGraphCompiler::computeDescriptorBindingPlan(RGCompiledGraph& compiled)
    {
        const auto& order = compiled.execution_order;
        if (order.empty()) return;

        constexpr uint32_t kMaxSlots = 16;
        std::array<VkPipelineLayout, kMaxSlots> slot_last_layout{};
        std::array<uint64_t,         kMaxSlots> slot_last_identity{};
        slot_last_layout.fill(VK_NULL_HANDLE);
        slot_last_identity.fill(0ull);

        auto resetSlotTracking = [&]() {
            slot_last_layout.fill(VK_NULL_HANDLE);
            slot_last_identity.fill(0ull);
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

            for (const PassDSBinding& binding : cpass.pass->ds_bindings)
            {
                const uint32_t slot = binding.slot;
                if (slot >= kMaxSlots) continue;

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
                }
                cpass.render.ds_bind_recipe.push_back(recipe);

                const bool force_rebind =
                    (binding.mode == EDSBindMode::PER_FIF)
                 || (binding.mode == EDSBindMode::VERSIONED)
                 || (binding.source == EDSBindingSource::Scene)
                 || (binding.source == EDSBindingSource::Transient);

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

        // 2. Traverse passes in execution order
        for (uint32_t pass_idx : compiled.execution_order)
        {
            RGCompiledPass& cpass = compiled.compiled_passes[pass_idx];
            const RGPassDescription* desc = cpass.pass;

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
                    
                    // If this is the first touch and it's IMPORTED with oldLayout UNDEFINED,
                    // then srcAccessMask must be 0 and srcStageMask can be TOP_OF_PIPE
                    if (!tracker.touched && tracker.current_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                        barrier2.srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                        barrier2.srcAccessMask = VK_ACCESS_2_NONE;
                    }

                    // Subresource range (Mip/Layer)
                    barrier2.subresourceRange.aspectMask = buildAspectMask(tex_desc.format);
                    barrier2.subresourceRange.baseMipLevel   = tex_ref.range.base_mip_level;
                    barrier2.subresourceRange.levelCount     = tex_ref.range.level_count;
                    barrier2.subresourceRange.baseArrayLayer = tex_ref.range.base_array_layer;
                    barrier2.subresourceRange.layerCount     = tex_ref.range.layer_count;

                    cpass.sync.image_barriers_2.push_back(barrier2);
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
            for (const auto& buf_ref : desc->buffers)
            {
                const uint32_t res_idx = buf_ref.resource.index;
                if (res_idx >= compiled.valid_resources.size() || !compiled.valid_resources[res_idx]) continue;

                VulkanResourceState target_state = determineBufferState_shared(buf_ref);
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
                    buf_barrier2.offset         = buf_ref.offset;
                    buf_barrier2.size           = (buf_ref.size == 0) ? VK_WHOLE_SIZE : buf_ref.size;
                    cpass.sync.buffer_barriers_2.push_back(buf_barrier2);
                }

                // Same WAR-accumulation rule as textures: on a read-after-read
                // with no barrier, OR the stage/access so a later writer waits on
                // every prior reader. First touch and barrier-emitting accesses
                // replace. (Buffers have no layout to track.)
                if (need_barrier || !was_touched)
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
                // Check if transition to final_layout is needed
                if (res.import_info->final_layout != VK_IMAGE_LAYOUT_UNDEFINED && 
                    resource_states[i].current_layout != res.import_info->final_layout) 
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

    // ---------------------------
    // 8) Compute parallel recording groups
    // ---------------------------
    void RenderGraphCompiler::computeParallelGroups(RGCompiledGraph& compiled)
    {
        compiled.parallel_groups.clear();

        const auto& passes  = compiled.compiled_passes;
        const auto& layout  = compiled.render_pass_layout;
        const auto& order   = compiled.execution_order;

        // Track which passes have been assigned to a group
        std::vector<uint8_t> assigned(passes.size(), 0);

        // Precompute per-pass read/write bitsets for O(1) dependency checks.
        const uint32_t resource_count = static_cast<uint32_t>(compiled.original_graph.resources.size());
        const uint32_t num_words = (resource_count + 63) / 64;
        std::vector<std::vector<uint64_t>> write_bits(passes.size(), std::vector<uint64_t>(num_words, 0));
        std::vector<std::vector<uint64_t>> read_bits(passes.size(), std::vector<uint64_t>(num_words, 0));
        for (uint32_t pi = 0; pi < static_cast<uint32_t>(passes.size()); ++pi)
        {
            for (uint32_t ri : passes[pi].resources.write_images)
                write_bits[pi][ri / 64] |= (1ULL << (ri % 64));
            for (uint32_t ri : passes[pi].resources.write_buffers)
                write_bits[pi][ri / 64] |= (1ULL << (ri % 64));
            for (uint32_t ri : passes[pi].resources.read_images)
                read_bits[pi][ri / 64] |= (1ULL << (ri % 64));
            for (uint32_t ri : passes[pi].resources.read_buffers)
                read_bits[pi][ri / 64] |= (1ULL << (ri % 64));
        }

        // Helper: check if pass B depends on pass A via shared resources (RAW, WAR, WAW)
        auto hasDataDependency = [&](uint32_t a, uint32_t b) -> bool {
            for (uint32_t w = 0; w < num_words; ++w)
            {
                const uint64_t wa = write_bits[a][w];
                const uint64_t wb = write_bits[b][w];
                const uint64_t ra = read_bits[a][w];
                const uint64_t rb = read_bits[b][w];
                if ((rb & wa) | (ra & wb) | (wa & wb)) return true;
            }
            return false;
        };

        // Strategy 1: Within the same RenderPass group, subpasses with no data
        // dependency can be recorded in parallel via secondary command buffers.
        for (uint32_t gi = 0; gi < static_cast<uint32_t>(layout.groups.size()); ++gi)
        {
            const auto& group = layout.groups[gi];
            if (group.passes.size() <= 1)
            {
                // Single subpass — still create a group entry (direct recording)
                if (!group.passes.empty())
                {
                    RGParallelGroup pg;
                    pg.pass_indices.push_back(group.passes[0].pass_index);
                    pg.render_pass_group     = gi;
                    pg.requires_secondary_cb = false;
                    compiled.parallel_groups.push_back(std::move(pg));
                    assigned[group.passes[0].pass_index] = 1;
                }
                continue;
            }

            // Multiple subpasses — check pairwise data dependencies
            // Collect subpasses that have NO data dependency on each other
            std::vector<uint32_t> parallel_set;
            for (const auto& sp : group.passes)
            {
                bool independent = true;
                for (uint32_t existing : parallel_set)
                {
                    if (hasDataDependency(existing, sp.pass_index) ||
                        hasDataDependency(sp.pass_index, existing))
                    {
                        independent = false;
                        break;
                    }
                }
                if (independent)
                    parallel_set.push_back(sp.pass_index);
            }

            RGParallelGroup pg;
            pg.pass_indices.assign(parallel_set.begin(), parallel_set.end());
            pg.render_pass_group     = gi;
            pg.requires_secondary_cb = (pg.pass_indices.size() > 1);
            for (uint32_t pi : pg.pass_indices)
                assigned[pi] = 1;
            compiled.parallel_groups.push_back(std::move(pg));
        }

        // Strategy 2: Compute / async passes that are independent of each other
        // can also form parallel groups. For now, each unassigned pass gets its
        // own single-entry group (conservative; can be merged later).
        for (uint32_t pi : order)
        {
            if (assigned[pi]) continue;

            RGParallelGroup pg;
            pg.pass_indices.push_back(pi);
            pg.render_pass_group     = std::numeric_limits<uint32_t>::max();
            pg.requires_secondary_cb = false;
            assigned[pi] = 1;
            compiled.parallel_groups.push_back(std::move(pg));
        }
    }

    // ---------------------------
    // 8.1) Force full binding for parallel passes
    // ---------------------------
    //
    // Secondary command buffers do NOT inherit pipeline or descriptor set state
    // from other secondary CBs in the same rendering scope.  The binding plans
    // computed by steps 5/5.1 assume serial state continuity within a render
    // pass group, which is invalid for the parallel path.
    //
    // For every parallel group that actually uses secondary CBs, force each
    // member pass to unconditionally bind its pipeline and all DS slots.
    void RenderGraphCompiler::forceFullBindingForParallelPasses(RGCompiledGraph& compiled)
    {
        for (const auto& pg : compiled.parallel_groups)
        {
            if (!pg.requires_secondary_cb) continue;

            for (const uint32_t pi : pg.pass_indices)
            {
                auto& cpass = compiled.compiled_passes[pi];
                if (cpass.render.pipeline != VK_NULL_HANDLE)
                    cpass.render.bind_pipeline = true;
                cpass.render.bind_ds_mask = ~0u;
            }
        }
    }

    // 9) Pre-compute per-group attachment resource indices and loadOps.
    //
    // For each render-pass group, record which logical resource index provides
    // the color and depth attachments.  Pre-compute VkAttachmentLoadOp: the
    // first group to write a resource in execution order gets CLEAR; every
    // subsequent group gets LOAD.
    //
    // This moves the per-frame O(groups×passes×textures) scan into a single
    // compile-time pass, and replaces the per-frame unordered_set tracking in
    // the recorder with a direct field read from RGRenderPassGroup.
    void RenderGraphCompiler::computeGroupLoadOps(RGCompiledGraph& compiled)
    {
        auto& groups = compiled.render_pass_layout.groups;

        // Always precompute attachment resource indices (needed for the runtime
        // fallback in conditional graphs too, so the recorder never scans passes).
        for (auto& group : groups)
        {
            for (const auto& gp : group.passes)
            {
                const auto& cpass = compiled.compiled_passes[gp.pass_index];
                if (!cpass.pass) continue;
                for (const auto& tr : cpass.pass->textures)
                {
                    if (tr.role == lux::common::ETextureRole::COLOR_ATTACHMENT &&
                        group.color_attachment_res_idx == std::numeric_limits<uint32_t>::max())
                        group.color_attachment_res_idx = tr.resource.index;
                    if (tr.role == lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT &&
                        group.depth_attachment_res_idx == std::numeric_limits<uint32_t>::max())
                        group.depth_attachment_res_idx = tr.resource.index;
                }
                if (group.color_attachment_res_idx != std::numeric_limits<uint32_t>::max() &&
                    group.depth_attachment_res_idx  != std::numeric_limits<uint32_t>::max())
                    break;
            }
        }

        // Assign CLEAR to the first group that writes each resource,
        // LOAD to all subsequent groups. Indexed by resource index.
        const size_t res_count = compiled.original_graph.resources.size();
        std::vector<bool> cleared_color(res_count, false);
        std::vector<bool> cleared_depth(res_count, false);

        // Pre-mark imported resources that can actually preserve prior content,
        // so their first consumer gets LOAD_OP_LOAD instead of LOAD_OP_CLEAR.
        // For UNDEFINED initial_layout, first write must CLEAR.
        for (size_t i = 0; i < res_count; ++i)
        {
            const auto& res = compiled.original_graph.resources[i];
            if (res.lifetime == ERGResourceLifetime::IMPORTED && res.import_info &&
                shouldPreserveFirstWriteLoadOp(
                    res.import_info->preserve_content,
                    res.import_info->initial_layout))
            {
                cleared_color[i] = true;
                cleared_depth[i] = true;
            }
        }

        std::vector<bool> group_visited(groups.size(), false);
        for (uint32_t pass_idx : compiled.execution_order)
        {
            if (pass_idx >= compiled.render_pass_layout.pass_to_group.size()) continue;
            const uint32_t group_idx = compiled.render_pass_layout.pass_to_group[pass_idx];
            if (group_idx == std::numeric_limits<uint32_t>::max()) continue;
            if (group_visited[group_idx]) continue;
            group_visited[group_idx] = true;

            auto& group = groups[group_idx];
            const uint32_t cr = group.color_attachment_res_idx;
            const uint32_t dr = group.depth_attachment_res_idx;

            if (cr < res_count)
            {
                group.color_load_op = cleared_color[cr] ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                        : VK_ATTACHMENT_LOAD_OP_CLEAR;
                cleared_color[cr] = true;
            }
            if (dr < res_count)
            {
                group.depth_load_op = cleared_depth[dr] ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                        : VK_ATTACHMENT_LOAD_OP_CLEAR;
                cleared_depth[dr] = true;
            }
        }
    }

    // 10) Classify conditional passes as Elective.
    //
    // A pass with a runtime condition lambda is "Intra-Group Additive" when its
    // render-pass group also contains at least one unconditional pass.  This
    // guarantees that:
    //   • The group's CLEAR loadOp is always emitted by the unconditional pass,
    //     so skipping the elective pass never changes the first-writer.
    //   • Same-group passes share identical attachment layouts, so skipping
    //     does not break the pre-computed barrier chain.
    //
    // Isolated conditional passes (sole member of their group, or the unique
    // first-writer of a resource) currently do NOT exist in the engine.
    // If one is ever added, this function should assert/error rather than
    // silently producing an incorrect runtime schedule.
    bool RenderGraphCompiler::classifyElectivePasses(RGCompiledGraph& compiled)
    {
        const auto& groups = compiled.render_pass_layout.groups;
        const uint32_t pass_count = static_cast<uint32_t>(compiled.compiled_passes.size());
        const uint32_t resource_count = static_cast<uint32_t>(compiled.original_graph.resources.size());

        // Precompute: for each resource, does any later pass read it?
        // resource_has_reader[ri] == true means at least one pass reads resource ri.
        std::vector<bool> resource_has_reader(resource_count, false);
        for (const auto& cpass : compiled.compiled_passes)
        {
            for (uint32_t ri : cpass.resources.read_images)
                if (ri < resource_count) resource_has_reader[ri] = true;
            for (uint32_t ri : cpass.resources.read_buffers)
                if (ri < resource_count) resource_has_reader[ri] = true;
        }

        for (auto& cpass : compiled.compiled_passes)
        {
            if (!cpass.condition)
            {
                cpass.elective_kind = EElectiveKind::NONE;
                continue;
            }

            // Find the render-pass group this pass belongs to.
            const uint32_t pi = cpass.pass_index;
            const uint32_t group_idx = (pi < compiled.render_pass_layout.pass_to_group.size())
                                       ? compiled.render_pass_layout.pass_to_group[pi]
                                       : std::numeric_limits<uint32_t>::max();

            if (group_idx == std::numeric_limits<uint32_t>::max())
            {
                // Non-graphics (compute/transfer) conditional pass has no render-pass
                // group, so runtime skip is only safe when it has no downstream readers.
                auto result = findFirstConsumedWrite(compiled, cpass.pass_index,
                                  cpass.resources.write_images, resource_has_reader);
                if (result.resource == std::numeric_limits<uint32_t>::max())
                    result = findFirstConsumedWrite(compiled, cpass.pass_index,
                                  cpass.resources.write_buffers, resource_has_reader);

                if (result.resource != std::numeric_limits<uint32_t>::max())
                {
                    const std::string producer_name = cpass.pass ? cpass.pass->name : "<null>";
                    const std::string consumer_name =
                        (result.consumer_pass < compiled.compiled_passes.size()
                         && compiled.compiled_passes[result.consumer_pass].pass)
                            ? compiled.compiled_passes[result.consumer_pass].pass->name
                            : "<null>";

                    compiled.compile_error =
                        "RenderGraphCompiler: conditional non-graphics pass '"
                        + producer_name
                        + "' writes resource #"
                        + std::to_string(result.resource)
                        + " consumed by downstream pass '"
                        + consumer_name
                        + "'. Conditional non-graphics producer with downstream readers is unsupported.";
                    return false;
                }

                cpass.elective_kind = EElectiveKind::INTRA_GROUP_ADDITIVE;
                continue;
            }

            const auto& group = groups[group_idx];
            bool has_unconditional_sibling = false;
            for (const auto& gp : group.passes)
            {
                const auto& sibling = compiled.compiled_passes[gp.pass_index];
                if (!sibling.condition)
                {
                    has_unconditional_sibling = true;
                    break;
                }
            }

            if (has_unconditional_sibling)
            {
                cpass.elective_kind = EElectiveKind::INTRA_GROUP_ADDITIVE;
            }
            else
            {
                compiled.compile_error =
                    "RenderGraphCompiler: render-pass group with all-conditional passes is unsupported."
                    " Offending pass: '" + cpass.pass->name + "'.";
                return false;
            }
        }
        return true;
    }

    // =====================================================================
    //  Phase 1: Kernel-driven fast-path compiler stages
    // =====================================================================

    // ---------------------------
    // 11) canBuildFastPath
    // ---------------------------
    //
    // Returns true when every live pass is eligible for compiled execution.
    // RecorderFallback passes force mixed mode but no longer prevent the
    // compiler from building execution products for other passes.

    bool RenderGraphCompiler::canBuildFastPath(const RGCompiledGraph& compiled)
    {
        bool has_live_pass = false;
        for (const auto& cpass : compiled.compiled_passes)
        {
            if (cpass.pass == nullptr)
                continue;
            has_live_pass = true;
            if (cpass.execution_mode == EPassExecutionMode::RECORDER_FALLBACK)
                return false;
        }
        return has_live_pass;
    }

    // ---------------------------
    // 11.1) computeMeshBucketLayout
    // ---------------------------
    //
    // Scans all compiled MeshDraw passes and generates one MeshLane per
    // MDC entry.  Lanes are sorted by (pass_index, pipeline, geometry_kind,
    // bucket_id) to minimise runtime pipeline switches.  Buffer offsets
    // are assigned sequentially.

    void RenderGraphCompiler::computeMeshBucketLayout(RGCompiledGraph& compiled,
                                                      PipelineManager& pipeline_manager)
    {
        MeshBucketLayoutPlan plan;

        // Collect lanes: each kernel with a contribute_mesh hook adds its lanes.
        for (uint32_t pi = 0; pi < static_cast<uint32_t>(compiled.compiled_passes.size()); ++pi)
        {
            const auto& cpass = compiled.compiled_passes[pi];
            if (!cpass.pass) continue;

            const auto* desc = KernelRegistry::instance().find(cpass.pass->kernel_id);
            if (desc && desc->contribute_mesh)
                desc->contribute_mesh(pi, cpass, plan, pipeline_manager);
        }

        // Sort lanes: (pass_index, pipeline, geometry_kind, bucket_id)
        // This groups identical pipelines together, minimising vkCmdBindPipeline calls.
        std::sort(plan.lanes.begin(), plan.lanes.end(), 
            [](const MeshLane& a, const MeshLane& b)
            {
                if (a.pass_index != b.pass_index)       return a.pass_index < b.pass_index;
                if (a.pipeline   != b.pipeline)         return a.pipeline < b.pipeline;
                if (a.geometry_kind != b.geometry_kind)  return a.geometry_kind < b.geometry_kind;
                return a.bucket_id < b.bucket_id;
            }
        );

        // Assign offsets matching cull shader addressing.
        // Each lane maps to exactly one MDC entry.
        // indirect_offset = mdc_index * 20, count_offset = mdc_index * 4, maxDraw = 1
        constexpr VkDeviceSize kIndirectCommandSize = 20; // sizeof(VkDrawIndexedIndirectCommand)
        VkPipeline prev_pipeline = VK_NULL_HANDLE;
        uint32_t   prev_pass     = ~0u;

        for (uint32_t i = 0; i < static_cast<uint32_t>(plan.lanes.size()); ++i)
        {
            auto& lane = plan.lanes[i];
            lane.lane_id         = i;
            lane.indirect_offset = static_cast<VkDeviceSize>(lane.mdc_index) * kIndirectCommandSize;
            lane.count_offset    = static_cast<VkDeviceSize>(lane.mdc_index) * sizeof(uint32_t);
            lane.bind_pipeline   = (lane.pipeline != prev_pipeline || lane.pass_index != prev_pass);
            prev_pipeline = lane.pipeline;
            prev_pass     = lane.pass_index;
        }

        // Size totals from max mdc_index
        uint32_t max_mdc = 0;
        for (const auto& lane : plan.lanes)
            max_mdc = std::max(max_mdc, lane.mdc_index + 1);
        plan.total_indirect_size = static_cast<VkDeviceSize>(max_mdc) * kIndirectCommandSize;
        plan.total_count_size    = static_cast<VkDeviceSize>(max_mdc) * sizeof(uint32_t);

        compiled.mesh_bucket_layout = std::move(plan);
    }

    // ---------------------------
    // 11.2) computeViewAllocatorPlan
    // ---------------------------
    //
    // Computes a per-view arena layout from the mesh bucket layout and
    // frustum data requirements.  Each region is aligned to a conservative
    // 256-byte boundary which satisfies all known Vulkan minimum offset
    // alignment requirements.

    void RenderGraphCompiler::computeViewAllocatorPlan(RGCompiledGraph& compiled)
    {
        constexpr VkDeviceSize kAlignment = 256; // Conservative min*OffsetAlignment

        auto alignUp = [](VkDeviceSize size, VkDeviceSize align) -> VkDeviceSize {
            return (size + align - 1) & ~(align - 1);
        };

        ViewAllocatorPlan plan;
        VkDeviceSize offset = 0;

        // Region 0: Mesh indirect buffer
        if (compiled.mesh_bucket_layout.has_value() && compiled.mesh_bucket_layout->total_indirect_size > 0)
        {
            ViewArenaRegion region{};
            region.region_id = static_cast<uint32_t>(plan.regions.size());
            region.offset    = offset;
            region.size      = compiled.mesh_bucket_layout->total_indirect_size;
            region.alignment = kAlignment;
            region.usage     = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            plan.regions.push_back(region);
            offset = alignUp(offset + region.size, kAlignment);
        }

        // Region 1: Mesh draw-count buffer
        if (compiled.mesh_bucket_layout.has_value() && compiled.mesh_bucket_layout->total_count_size > 0)
        {
            ViewArenaRegion region{};
            region.region_id = static_cast<uint32_t>(plan.regions.size());
            region.offset    = offset;
            region.size      = alignUp(compiled.mesh_bucket_layout->total_count_size, 4);
            region.alignment = kAlignment;
            region.usage     = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                             | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            plan.regions.push_back(region);
            offset = alignUp(offset + region.size, kAlignment);
        }

        // Region 2: Frustum UBO data (96 bytes per frustum x number of cull passes)
        {
            constexpr VkDeviceSize kFrustumSize = 96; // 6 planes x 16 bytes

            // Accumulate arena requirements from all registered kernels.
            ViewArenaContribution arena_accum{};
            for (const auto& pass : compiled.original_graph.passes)
            {
                const auto* desc = KernelRegistry::instance().find(pass.kernel_id);
                if (desc && desc->contribute_arena)
                    desc->contribute_arena(pass, arena_accum);
            }

            const VkDeviceSize frustum_total =
                kFrustumSize * arena_accum.frustum_ubo_count
                + kFrustumSize * arena_accum.shadow_slice_count;
            if (frustum_total > 0)
            {
                ViewArenaRegion region{};
                region.region_id = static_cast<uint32_t>(plan.regions.size());
                region.offset    = offset;
                region.size      = frustum_total;
                region.alignment = kAlignment;
                region.usage     = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                plan.regions.push_back(region);
                offset = alignUp(offset + region.size, kAlignment);
            }
        }

        plan.total_arena_size = offset;
        plan.max_views = 4;   // Conservative default; can be tuned per renderer

        compiled.view_allocator_plan = std::move(plan);
    }

    // ---------------------------
    // 11.3) buildBarrierProgram
    // ---------------------------
    //
    // Converts the per-pass prebuilt barrier arrays into a BarrierProgram with
    // separate first-view and subsequent-view sequences.  For subsequent views,
    // the imported resource's oldLayout is patched using imported_final_states.

    void RenderGraphCompiler::buildBarrierProgram(RGCompiledGraph& compiled)
    {
        BarrierProgram program;
        lux::cxx::SmallVector<uint8_t, 64> subsequent_view_patched;
        if (!compiled.original_graph.resources.empty())
        {
            subsequent_view_patched.assign(compiled.original_graph.resources.size(), 0u);
        }

        for (uint32_t exec_idx = 0; exec_idx < static_cast<uint32_t>(compiled.execution_order.size()); ++exec_idx)
        {
            const uint32_t pi = compiled.execution_order[exec_idx];
            const auto& sync = compiled.compiled_passes[pi].sync;

            if (sync.prebuilt_image_barriers.empty() && sync.prebuilt_buffer_barriers.empty())
                continue;

            // First-view barriers: verbatim copy of prebuilt barriers
            {
                BarrierProgram::BarrierGroup group{};
                group.pass_index = pi;
                group.image_barriers.assign(sync.prebuilt_image_barriers.begin(),
                                            sync.prebuilt_image_barriers.end());
                group.buffer_barriers.assign(sync.prebuilt_buffer_barriers.begin(),
                                             sync.prebuilt_buffer_barriers.end());
                group.image_patch_resource_idx.assign(sync.image_patch_resource_idx.begin(),
                                                      sync.image_patch_resource_idx.end());
                group.buffer_patch_resource_idx.assign(sync.buffer_patch_resource_idx.begin(),
                                                       sync.buffer_patch_resource_idx.end());
                program.first_view_barriers.push_back(std::move(group));
            }

            // Subsequent-view barriers: patch the first touch per imported resource
            // to match the previous view's deterministic final state.
            {
                BarrierProgram::BarrierGroup group{};
                group.pass_index = pi;
                group.buffer_barriers.assign(sync.prebuilt_buffer_barriers.begin(),
                                             sync.prebuilt_buffer_barriers.end());
                group.buffer_patch_resource_idx.assign(sync.buffer_patch_resource_idx.begin(),
                                                       sync.buffer_patch_resource_idx.end());

                for (size_t bi = 0; bi < sync.prebuilt_image_barriers.size(); ++bi)
                {
                    VkImageMemoryBarrier2 barrier = sync.prebuilt_image_barriers[bi];
                    const uint32_t ri = sync.image_patch_resource_idx[bi];

                    const bool can_patch_first_touch = ri < subsequent_view_patched.size()
                                                    && !subsequent_view_patched[ri]
                                                    && ri < compiled.imported_final_state_lut.size();
                    if (can_patch_first_touch)
                    {
                        const uint32_t lut = compiled.imported_final_state_lut[ri];
                        if (lut != RGCompiledGraph::kInvalidSlotIdx &&
                            lut < compiled.imported_final_states.size())
                        {
                            barrier.srcStageMask = compiled.imported_final_states[lut].stage_mask;
                            barrier.srcAccessMask = compiled.imported_final_states[lut].access_mask;
                            barrier.oldLayout = compiled.imported_final_states[lut].layout;
                            subsequent_view_patched[ri] = 1u;
                        }
                    }
                    group.image_barriers.push_back(barrier);
                    group.image_patch_resource_idx.push_back(ri);
                }
                program.subsequent_view_barriers.push_back(std::move(group));
            }
        }

        // Final barriers
        if (!compiled.prebuilt_final_barriers.empty())
        {
            BarrierProgram::BarrierGroup final_group{};
            final_group.pass_index = ~0u;
            final_group.image_barriers.assign(compiled.prebuilt_final_barriers.begin(),
                                              compiled.prebuilt_final_barriers.end());
            final_group.image_patch_resource_idx.assign(compiled.final_patch_resource_idx.begin(),
                                                        compiled.final_patch_resource_idx.end());
            program.final_barriers.push_back(std::move(final_group));
        }

        // Build precomputed pass→barrier-group lookup tables so that
        // replayExecutionRange does not need to rebuild them each frame.
        {
            const uint32_t pass_count = static_cast<uint32_t>(compiled.compiled_passes.size());
            auto buildIndex = [&](const std::vector<BarrierProgram::BarrierGroup>& groups)
            {
                std::vector<uint32_t> idx(pass_count, RGCompiledGraph::kInvalidSlotIdx);
                for (uint32_t gi = 0; gi < static_cast<uint32_t>(groups.size()); ++gi)
                {
                    if (groups[gi].pass_index < pass_count)
                        idx[groups[gi].pass_index] = gi;
                }
                return idx;
            };
            program.first_view_group_by_pass = buildIndex(program.first_view_barriers);
            program.subsequent_view_group_by_pass = buildIndex(program.subsequent_view_barriers);
        }

        compiled.barrier_program = std::move(program);
    }

    // ---------------------------
    // 11.4) buildQueueSubmitProgram
    // ---------------------------
    //
    // Converts the per-queue execution orders and cross-queue dependencies
    // into a sequence of SubmissionTemplate entries.

    void RenderGraphCompiler::buildQueueSubmitProgram(RGCompiledGraph& compiled)
    {
        QueueSubmitProgram program;
        const auto& mqi = compiled.multi_queue_info;

        auto makeSubmission = [&](ERGQueueType queue, const std::vector<uint32_t>& order)
        {
            if (order.empty()) return;

            QueueSubmitProgram::SubmissionTemplate tmpl{};
            tmpl.queue = queue;

            for (uint32_t pi : order)
            {
                const auto& cpass = compiled.compiled_passes[pi];
                for (const auto& dep : cpass.wait_dependencies)
                {
                    VkSemaphoreSubmitInfo wait_info{};
                    wait_info.sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
                    wait_info.semaphore   = dep.semaphore;
                    wait_info.value       = dep.signal_value;
                    wait_info.stageMask   = dep.wait_stage;
                    tmpl.wait_templates.push_back(wait_info);
                }
                for (const auto& dep : cpass.signal_dependencies)
                {
                    VkSemaphoreSubmitInfo signal_info{};
                    signal_info.sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
                    signal_info.semaphore   = dep.semaphore;
                    signal_info.value       = dep.signal_value;
                    signal_info.stageMask   = dep.signal_stage;
                    tmpl.signal_templates.push_back(signal_info);
                }
            }

            program.submissions.push_back(std::move(tmpl));
        };

        makeSubmission(ERGQueueType::COMPUTE,  mqi.compute_order);
        makeSubmission(ERGQueueType::TRANSFER, mqi.transfer_order);
        makeSubmission(ERGQueueType::GRAPHICS, mqi.graphics_order);

        compiled.queue_submit_program = std::move(program);
    }

    // ---------------------------
    // 11.5) buildExecutionProgram
    // ---------------------------
    //
    // Generates the bytecode-style ExecutionProgram from the compiled pass
    // sequence.  Each compiled pass is translated into a linear sequence of
    // Command entries with arguments stored in a flat data blob.

    void RenderGraphCompiler::buildExecutionProgram(RGCompiledGraph& compiled)
    {
        ExecutionProgram program;
        program.pass_spans.resize(compiled.compiled_passes.size());
        const bool full_fast_path = canBuildFastPath(compiled);

        ProgramEmitter emitter{program, compiled};

        for (uint32_t exec_idx = 0; exec_idx < static_cast<uint32_t>(compiled.execution_order.size()); ++exec_idx)
        {
            const uint32_t pi = compiled.execution_order[exec_idx];
            const auto& cpass = compiled.compiled_passes[pi];
            if (!cpass.pass) continue;
            if (cpass.execution_mode == EPassExecutionMode::RECORDER_FALLBACK)
                continue;

            const uint32_t span_start = static_cast<uint32_t>(program.commands.size());

            const bool is_graphics =
                cpass.pass->type == ERGPassType::GRAPHICS;

            // --- Set pass context (executor tracks current pass) ---
            emitter.emit(ExecutionProgram::Command::EType::SetPassContext,
                        &pi, sizeof(uint32_t));

            // --- Pre-pass barriers ---
            const auto& sync = cpass.sync;
            if (!sync.prebuilt_image_barriers.empty() || !sync.prebuilt_buffer_barriers.empty())
            {
                struct { uint32_t pass_index; uint32_t phase; } bd{pi, 0};
                emitter.emit(ExecutionProgram::Command::EType::PipelineBarrier,
                            &bd, static_cast<uint16_t>(sizeof(bd)));
            }

            // --- Begin rendering (with prebuilt template) ---
            if (cpass.render.begin_render_pass)
                emitBeginRendering(emitter, pi, cpass, compiled);

            // --- Bind pipeline ---
            const bool emit_pass_pipeline = full_fast_path
                ? cpass.render.bind_pipeline
                : (cpass.render.pipeline != VK_NULL_HANDLE);
            if (emit_pass_pipeline && cpass.render.pipeline != VK_NULL_HANDLE)
            {
                struct { VkPipeline pipeline; VkPipelineLayout layout; }
                    bp{cpass.render.pipeline, cpass.render.pipeline_layout};
                emitter.emit(ExecutionProgram::Command::EType::BindPipeline,
                            &bp, static_cast<uint16_t>(sizeof(bp)));
            }

            // --- Bind descriptor sets ---
            for (const auto& recipe : cpass.render.ds_bind_recipe)
            {
                struct { uint32_t slot; VkDescriptorSet set; }
                    bd{recipe.slot, recipe.immutable_set};
                const uint32_t cmd_idx = emitter.emit(
                    ExecutionProgram::Command::EType::BindDescriptorSets,
                    &bd, static_cast<uint16_t>(sizeof(bd)));

                if (recipe.resolve != DSBindRecipe::resolveImmutable)
                {
                    ExecutionProgram::DynamicPatch patch{};
                    patch.command_index     = cmd_idx;
                    patch.data_field_offset = static_cast<uint16_t>(sizeof(uint32_t)); // offset of 'set' field
                    patch.source            = ExecutionProgram::DynamicPatch::ESource::FrameIndex;
                    patch.source_param      = recipe.slot;
                    program.patches.push_back(patch);
                }
            }

            // --- Push constants (graphics: scene_index + view_index) ---
            if (is_graphics && !cpass.render.ds_bind_recipe.empty()
                && cpass.render.pipeline_layout != VK_NULL_HANDLE)
            {
                emitter.emit(ExecutionProgram::Command::EType::PushConstants,
                            &pi, sizeof(uint32_t));
            }

            // --- Viewport / Scissor (every graphics pass) ---
            if (is_graphics && !cpass.pass->manual_viewport)
            {
                emitter.emit(ExecutionProgram::Command::EType::SetViewport,
                            &pi, sizeof(uint32_t));
                emitter.emit(ExecutionProgram::Command::EType::SetScissor,
                            &pi, sizeof(uint32_t));
            }

            // --- Kernel-specific commands ---
            const uint32_t kernel_cmd_start = static_cast<uint32_t>(program.commands.size());
            if (cpass.execution_mode == EPassExecutionMode::COMPILED_CALLBACK)
            {
                emitter.emit(ExecutionProgram::Command::EType::InvokeKernelFn, nullptr, 0);
            }
            else
            {
                // Registry dispatch — O(1) lookup, zero coupling to specific kernels.
                const auto* desc = KernelRegistry::instance().find(cpass.pass->kernel_id);
                if (desc && desc->emit)
                    desc->emit(emitter, pi, cpass, compiled);
            }

            // Safety net: if a pass is classified as native but no kernel body command
            // was emitted, fall back to callback invocation when available.
            if (cpass.execution_mode == EPassExecutionMode::COMPILED_NATIVE
                && cpass.pass->kernel_fn
                && static_cast<uint32_t>(program.commands.size()) == kernel_cmd_start)
            {
                emitter.emit(ExecutionProgram::Command::EType::InvokeKernelFn, nullptr, 0);
            }

            // --- End rendering ---
            if (cpass.render.end_render_pass)
            {
                emitter.emit(ExecutionProgram::Command::EType::EndRendering, nullptr, 0);
            }

            auto& span = program.pass_spans[pi];
            span.first_command = span_start;
            span.one_past_last_command = static_cast<uint32_t>(program.commands.size());
            span.valid = span.one_past_last_command > span_start;
        }

        program.full_fast_path = full_fast_path;

        // Verify dynamic-rendering compatibility: no PipelineBarrier inside
        // a [BeginRendering, EndRendering) scope.
        {
            bool inside_scope = false;
            bool compatible   = true;
            for (const auto& cmd : program.commands)
            {
                if (cmd.type == ExecutionProgram::Command::EType::BeginRendering)
                    inside_scope = true;
                else if (cmd.type == ExecutionProgram::Command::EType::EndRendering)
                    inside_scope = false;
                else if (cmd.type == ExecutionProgram::Command::EType::PipelineBarrier && inside_scope)
                {
                    compatible = false;
                    break;
                }
            }
            program.dynamic_rendering_compatible = compatible;
        }

        compiled.execution_program = std::move(program);
    }

} // namespace lux::render
