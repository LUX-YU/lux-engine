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
        EPassExecutionMode classifyPassExecutionMode(const RGPassDescription* pass_desc) noexcept
        {
            if (pass_desc == nullptr)
            {
                return EPassExecutionMode::RECORDER_FALLBACK;
            }
            // 注意:condition 不降级执行模式。降级曾让"纯 setKernel + 条件"
            // 的 pass(高亮链的 Cull/MaskDraw)拿不到 span,而 fallback 路径
            // 对无 recorder/kernel_fn 的 pass 是 no-op —— 链"运行"却什么都
            // 不录(选中高亮整链失效)。条件跳过由 serial 路径的 cond_skip
            // 快照负责;fast path 由 canBuildFastPath 显式排除条件图。
            if (!pass_desc->hasKernel())
            {
                return EPassExecutionMode::RECORDER_FALLBACK;
            }

            // Prefer callback execution when a pass provides kernel_fn.
            if (pass_desc->kernel_fn)
            {
                return EPassExecutionMode::COMPILED_CALLBACK;
            }

            // CompiledNative iff the kernel has a registered emit function.
            const auto* desc = KernelRegistry::instance().find(pass_desc->kernel_id);
            if (desc && desc->emit)
            {
                return EPassExecutionMode::COMPILED_NATIVE;
            }

            return EPassExecutionMode::RECORDER_FALLBACK;
        }

        void classifyExecutionModes(RGCompiledGraph& compiled)
        {
            for (std::uint32_t pass_index = 0;
                 pass_index < compiled.compiled_passes.size();
                 ++pass_index)
            {
                auto& cpass = compiled.compiled_passes[pass_index];
                cpass.execution_mode = classifyPassExecutionMode(cpass.pass);

                // 守卫:纯 setKernel 的 pass(无 recorder/kernel_fn)落到
                // RECORDER_FALLBACK 意味着 recordPassContent 对它 no-op——
                // 声明了绘制意图却永远不执行。选中高亮曾因此整链静默失效
                // (condition 降级执行模式的历史行为),响亮报出来。
                if (cpass.execution_mode == EPassExecutionMode::RECORDER_FALLBACK &&
                    cpass.pass && cpass.pass->hasKernel() &&
                    !cpass.pass->recorder && !cpass.pass->kernel_fn)
                {
                    compiled.diagnostics.push_back(
                        renderError<err::graph::KernelEmitterMissing>(pass_index));
                }
            }
        }

        bool hasCompiledPasses(const RGCompiledGraph& compiled) noexcept
        {
            return std::any_of(compiled.compiled_passes.begin(), compiled.compiled_passes.end(),
                [](const RGCompiledPass& cpass)
                {
                    return cpass.execution_mode != EPassExecutionMode::RECORDER_FALLBACK;
                });
        }

        // Scan resources and build fast lookup tables for resize-tracking,
        // dynamic-import refresh and slot-based injection.
        void buildResourceLookupTables(RGCompiledGraph& compiled)
        {
            const auto& resources = compiled.original_graph.resources;
            const size_t res_count = resources.size();
            std::vector<bool> added_to_relative(res_count, false);

            for (size_t i = 0; i < res_count; ++i)
            {
                const auto& res = resources[i];
                const auto ri = static_cast<uint32_t>(i);

                if (res.type == ERGResourceType::TEXTURE)
                {
                    const auto& tex_desc = std::get<RGTextureDescription>(res.desc);
                    if (tex_desc.sizing_mode == ERGSizeMode::RELATIVE_MODE)
                    {
                        compiled.relative_size_resources.push_back(ri);
                        added_to_relative[i] = true;
                    }
                }

                if (res.lifetime != ERGResourceLifetime::IMPORTED)
                {
                    continue;
                }

                const bool has_dynamic_getter =
                    (res.import_info && res.import_info->image_getter != nullptr) ||
                    (res.import_buffer_info && res.import_buffer_info->buffer_getter != nullptr);
                if (has_dynamic_getter)
                {
                    compiled.dynamic_external_resources.push_back(ri);
                }

                if (!res.import_info || !res.import_info->slot.has_value())
                {
                    continue;
                }

                const size_t slot_idx = static_cast<size_t>(*res.import_info->slot);
                if (slot_idx >= kTargetSlotCount)
                {
                    continue;
                }

                if (!added_to_relative[i])
                {
                    compiled.relative_size_resources.push_back(ri);
                    added_to_relative[i] = true;
                }

                if (compiled.valid_resources[ri])
                {
                    compiled.slot_resource_idx[slot_idx] = ri;
                }
            }
        }

    } // namespace


    namespace
    {
        // ── 资源用途并集 ────────────────────────────────────────────
        //
        // 此前纹理的 usage 只认创建者一面之词(分配器
        // `image_info.usage = convertTextureUsage(tex_desc.usage)` 不看消费方)。
        // 于是下游想以新角色消费一个既有资源 —— 外部 AO 采样 G-buffer 是典型 ——
        // 必须回头改创建者:耦合方向是反的。并集后"谁需要什么用途"由需要的人
        // 用 pass 角色声明,分配器照旧只读描述(lazy 判定天然发生在并集之后)。

        /// 纹理角色 → 用途位。INPUT_ATTACHMENT 属附件类用途 —— 它不把资源拖出
        /// lazy(分配器的 kAttachmentOnlyUsage 含它),local_read 的 G-buffer
        /// 因此继续零显存。
        [[nodiscard]] ERGTextureUsageFlags usageBitsForRole(lux::render::ETextureRole role) noexcept
        {
            using B = ERGTextureUsageBits;
            switch (role)
            {
            case lux::render::ETextureRole::SAMPLED:                  return static_cast<ERGTextureUsageFlags>(B::SAMPLED);
            case lux::render::ETextureRole::COLOR_ATTACHMENT:         return static_cast<ERGTextureUsageFlags>(B::COLOR_ATTACHMENT);
            case lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT: return static_cast<ERGTextureUsageFlags>(B::DEPTH_STENCIL);
            case lux::render::ETextureRole::INPUT_ATTACHMENT:         return static_cast<ERGTextureUsageFlags>(B::INPUT_ATTACHMENT);
            case lux::render::ETextureRole::UNORDERED_ACCESS:         return static_cast<ERGTextureUsageFlags>(B::STORAGE);
            default:                                                  return static_cast<ERGTextureUsageFlags>(0);
            }
        }

        void unionTextureUsageFromRoles(RGCompiledGraph& compiled)
        {
            auto& graph = compiled.original_graph;
            constexpr ERGTextureUsageFlags kAttachmentOnly =
                  static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::COLOR_ATTACHMENT)
                | static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::DEPTH_STENCIL)
                | static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::INPUT_ATTACHMENT);

            std::vector<ERGTextureUsageFlags> role_union(graph.resources.size(), ERGTextureUsageFlags{0});
            for (const auto& pass : graph.passes)
                for (const auto& ref : pass.textures)
                    if (ref.resource.index < role_union.size())
                    {
                        role_union[ref.resource.index] |= usageBitsForRole(ref.role);
                    }

            for (size_t i = 0; i < graph.resources.size(); ++i)
            {
                auto& res = graph.resources[i];
                if (res.type != ERGResourceType::TEXTURE)
                {
                    continue;
                }
                auto* tex = std::get_if<RGTextureDescription>(&res.desc);
                if (tex == nullptr)
                {
                    continue;
                }
                // 导入资源的物理镜像不归图分配,usage 由导入方定 —— 不动。
                if (res.import_info)
                {
                    continue;
                }

                ERGTextureUsageFlags add = role_union[i] & ~tex->usage;
                if (add == ERGTextureUsageFlags{0})
                    continue;

                if (tex->keep_transient && (add & ~kAttachmentOnly) != 0)
                {
                    // 创建者主权:承诺了仅附件用途,越界需求丢弃 + 记一条诊断。
                    compiled.diagnostics.push_back(
                        renderError<err::graph::TransientUsageViolation>(
                            static_cast<std::uint32_t>(i),
                            static_cast<std::uint32_t>(add & ~kAttachmentOnly)));
                    add &= kAttachmentOnly;
                    if (add == ERGTextureUsageFlags{0})
                        continue;
                }

                // 当前树上这条应当为零 —— validation 一直干净意味着创建者声明 ⊇
                // 消费角色。它开火 = 抓到一处此前的欠声明,值得看一眼。
                compiled.diagnostics.push_back(
                    renderError<err::graph::UsageUnderdeclared>(
                        static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(add)));
                tex->usage |= add;
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

        // 0.2) 用途并集(见上方函数注释)。必须在 forward-ref 解析之后
        //      (占位句柄此前索引不到资源)、一切分配之前。
        unionTextureUsageFromRoles(compiled);

        // 0.5) Make bindResourceDS-declared external consumption visible to the
        //      graph: inject each declared_consume handle as a READ so dependency
        //      analysis (next), ordering and dead-pass elimination account for it.
        //      Must run AFTER forward-ref resolution (so declared handles are real)
        //      and BEFORE buildGlobalInfo (which runs the dependency analyzer).
        injectResourceDSDependencies(compiled);

        // 1) Global analysis: dependencies, lifetimes, resource validity, render pass layout
        if (!buildGlobalInfo(compiled, options))
        {
            return compiled;
        }

        // 1.5) Auto-fill transient-DS image layouts from each binding pass's resource
        //      role, so the descriptor layout always equals the layout computeBarriers
        //      transitions the image to (no hand-written, drift-prone layouts). Runs
        //      after forward-ref resolution + DS-write remap (resources are real) and
        //      before descriptor-set materialisation.
        autoFillTransientDSLayouts(compiled);

        // 1.6) Warn about bindResourceDS bindings that don't declare the resource
        //      they consume — invisible to ordering / dead-pass elimination.
        //      Phase A: warn only (a later milestone makes this a hard error).
        validateResourceDSBindings(compiled);

        // 1.7) Graph-compile-time descriptor layout allocation: aggregate the
        //      per-binding reflection across the whole graph into a
        //      LayoutPlan. Must run before buildCompiledPasses — that step
        //      builds the PSOs and snapshots tmpl.pipeline_layout into the
        //      compiled pass.
        if (!computeGraphDescriptorLayouts(compiled, pipeline_manager))
        {
            return compiled;
        }

        // 2) Build compilation info for each pass (render pass / framebuffer / pipeline / resource mapping)
        if (!buildCompiledPasses(compiled, pipeline_manager))
        {
            return compiled;
        }

        // 3) Execution order = topological order
        computeExecutionOrder(compiled);

        // 3.05) Starved pass elimination: disable passes whose INPUTS were never
        //       produced (unresolved optional references), transitively. Runs
        //       BEFORE dead-pass elimination so a starved pass does not keep its
        //       own producers alive through that BFS.
        eliminateStarvedPasses(compiled);

        // 3.1) Dead pass elimination (A-02): remove passes whose outputs contribute
        //      to no exported/imported resource.  Must run before barrier computation
        //      so that dead passes don't generate unnecessary barriers.
        eliminateDeadPasses(compiled);

        // 3.5) Assign target queue based on pass type
        computeQueueAssignment(compiled);

        // 3.6) Cross-queue dependencies: partition per-queue, compute timeline
        //      semaphore sync points (A-01). Also fail-fasts (sets compile_error) if
        //      the graph needs a queue-family ownership transfer we cannot emit.
        computeCrossQueueDependencies(compiled);
        if (!compiled.compile_error.ok())
        {
            return compiled; // cross-queue EXCLUSIVE resource sharing needs unimplemented QFOT
        }

        // 4) Based on execution order, compute Begin/End RenderPass for each graphics pass
        computeRenderPassBoundaries(compiled);

        // ── 活 pass 资源访问反查索引:dead-pass 消除后构建一次,4.5 与 9
        //    共同消费(F6;各 phase 私建索引的语义漂移收口)。
        const LiveAccessIndex access_index = buildLiveAccessIndex(compiled);

        // 4.5) Classify conditional passes as elective (intra-group additive)
        //      so the binding plan steps can conservatively reset after them.
        if (!classifyElectivePasses(compiled, access_index))
        {
            return compiled;
        }

        // 5) Based on execution order and render pass boundaries, compute minimum necessary BindPipeline timing
        computePipelineBindingPlan(compiled);

        // 5.1) Per-slot descriptor set layout-break mask (feeds runtime DS dedup)
        computeDescriptorBindingPlan(compiled, pipeline_manager, options.domain_sets);

        // Attachment load/store decisions are also synchronization inputs: a
        // LOAD operation performs an attachment read at vkCmdBeginRendering.
        // Compute them before barriers so the destination access mask can
        // include that read instead of describing only the following writes.
        computeAttachmentOps(compiled, access_index);
        if (!compiled.compile_error.ok())
        {
            return compiled;   // F13 guard: conditional CLEAR in an lr write chain
        }

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

        // (并行录制组已删除:computeParallelGroups 的产物从未被录制器消费
        //  ——RGVulkanRecorder 无 secondary-CB 路径;而配套的
        //  forceFullBindingForParallelPasses 还为这条不存在的路径把最小
        //  绑定计划整组作废,串行录制纯亏。secondary CB 真做时按届时的
        //  录制器结构重写,git 历史可考。F7)

        // 10.5) Classify which passes can use compiled execution.
        classifyExecutionModes(compiled);

        // ===== 内核驱动的快路编译产物 =====
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
        // Dependency analysis consumes a backend-neutral view. Building the
        // small view array is graph-compile cold-path work; resource/pass
        // payloads remain owned by original_graph and are not copied.
        std::vector<RGLogicalPassView> logical_passes;
        logical_passes.reserve(compiled.original_graph.passes.size());
        for (const auto& pass : compiled.original_graph.passes)
        {
            logical_passes.push_back(RGLogicalPassView{
                .name = pass.name,
                .phase_mask = pass.phase_mask,
                .stage = pass.stage,
                .textures = std::span<const RGPassTextureRef>{
                    pass.textures.data(),
                    pass.textures.size()
                },
                .buffers = std::span<const RGPassBufferRef>{
                    pass.buffers.data(),
                    pass.buffers.size()
                },
                .after_passes = std::span<const std::string>{
                    pass.after_passes.data(),
                    pass.after_passes.size()
                },
                .before_passes = std::span<const std::string>{
                    pass.before_passes.data(),
                    pass.before_passes.size()
                }
            });
        }
        compiled.dependency_info = DependencyAnalyzer::analyze(
            RGLogicalGraphView{
                .resource_count = static_cast<std::uint32_t>(
                    compiled.original_graph.resources.size()
                ),
                .passes = logical_passes
            }
        );
        if (compiled.dependency_info.has_cycle)
        {
            compiled.compile_error = renderError<err::graph::DependencyCycle>();
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
                    compiled.compile_error = renderError<err::graph::ImportedResourceIncomplete>(
                        static_cast<std::uint32_t>(i));
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
                // Mark invalid so dead-pass elimination prunes any reader.
                // a REQUIRED reference left unresolved is a hard error (fail fast),
                // not a silent degrade to a null view / downstream VUID. Optional keeps
                // the prune behaviour (consumer degrades).
                if (res.reference_mode == ERGReference::Required) {
                    compiled.compile_error = renderError<err::graph::ReferencedResourceHasNoProducer>(
                        static_cast<std::uint32_t>(i));
                    return false;
                }
                compiled.valid_resources[i] = false;
            } else if (res.lifetime == ERGResourceLifetime::EXTERNAL) {
                // Feature-owned: no RG-backed image/buffer. Mark invalid so
                // computeBarriers skips it (the per-resource valid_resources guard
                // at the top of its texture/buffer loops means NO barrier is emitted
                // against a null handle). Dependency analysis + dead-pass elimination
                // read pass.textures/buffers directly (not valid_resources), so the
                // injected READ is still visible for ordering/reachability.
                compiled.valid_resources[i] = false;
            } else {
                // TRANSIENT / PERSISTENT → valid if description exists
                compiled.valid_resources[i] = true;
            }
        }

        // RenderPass layout (render pass groups organized by key)
        compiled.render_pass_layout = RenderPassPlanner::plan(
            compiled.original_graph,
            compiled.dependency_info,
            options.max_color_attachments
        );
        if (!compiled.render_pass_layout.valid)
        {
            compiled.compile_error = compiled.render_pass_layout.error;
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
            {
                return false;
            }
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
        {
            cpass.condition = &pass_desc.condition;
        }

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
            {
                return false;
            }
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
        {
            return;
        }

        const uint32_t group_index = layout.pass_to_group[pass_index];
        if (group_index == std::numeric_limits<uint32_t>::max())
        {
            return; // No layout, meaning this pass has no valid render pass configuration
        }

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
                {
                    return pass_desc.pipeline_variant_features[variant_index];
                }
                return 0u;
            };

            uint32_t subpass_index = 0;
            if (pass_index < layout.pass_to_subpass.size())
            {
                subpass_index = layout.pass_to_subpass[pass_index];
            }

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
                {
                    return;
                }
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
            compiled.compile_error = renderError<err::graph::ComputePassMissingPipeline>(cpass.pass_index);
            return false;
        }

        cpass.render.pipeline        = pipeline_manager.getComputePipeline(pass_desc.compute_pipeline_handle);
        cpass.render.pipeline_layout = pipeline_manager.getComputeLayout(pass_desc.compute_pipeline_handle);
        if (cpass.render.pipeline == VK_NULL_HANDLE || cpass.render.pipeline_layout == VK_NULL_HANDLE)
        {
            compiled.compile_error = renderError<err::graph::ComputePassPipelineStale>(cpass.pass_index);
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
            {
                continue;
            }

            const bool is_read =
                tex_ref.usage == ERGResourceUsage::READ ||
                tex_ref.usage == ERGResourceUsage::READ_WRITE;

            const bool is_write =
                tex_ref.usage == ERGResourceUsage::WRITE ||
                tex_ref.usage == ERGResourceUsage::READ_WRITE;

            if (is_read)
            {
                cpass.resources.read_images.push_back(res_idx);
            }
            if (is_write)
            {
                cpass.resources.write_images.push_back(res_idx);
            }
        }
    }

    void RenderGraphCompiler::mapPassBuffers(const RGCompiledGraph &compiled, RGCompiledPass &cpass)
    {
        const auto &pass = *cpass.pass;

        for (const auto &buf_ref : pass.buffers)
        {
            const uint32_t res_idx = buf_ref.resource.index;
            if (res_idx >= compiled.valid_resources.size() || !compiled.valid_resources[res_idx])
            {
                continue;
            }

            const bool is_read =
                buf_ref.usage == ERGResourceUsage::READ ||
                buf_ref.usage == ERGResourceUsage::READ_WRITE;

            const bool is_write =
                buf_ref.usage == ERGResourceUsage::WRITE ||
                buf_ref.usage == ERGResourceUsage::READ_WRITE;

            if (is_read)
            {
                cpass.resources.read_buffers.push_back(res_idx);
            }
            if (is_write)
            {
                cpass.resources.write_buffers.push_back(res_idx);
            }
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
            {
                forward_refs.push_back(i);
            }
            else
                actual_resources[graph.resources[i].name] = i;
        }

        if (forward_refs.empty())
        {
            return;
        }

        // Step 2: Build forward-ref index → actual index mapping
        std::vector<uint32_t> remap(resource_count);
        for (uint32_t i = 0; i < resource_count; ++i)
        {
            remap[i] = i; // identity by default
        }

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
                {
                    tex.resource.index = remap[tex.resource.index];
                }
            }
            for (auto& buf : pass.buffers)
            {
                if (remap[buf.resource.index] != buf.resource.index)
                {
                    buf.resource.index = remap[buf.resource.index];
                }
            }
        }

        // Transient-DS writes also reference resources by handle (e.g. a DS built
        // from builder.referenceTexture("SceneDepth")). Without this remap a write
        // could keep pointing at the unresolved forward-ref index while the pass's
        // own .read() got remapped to the real one — so autoFillTransientDSLayouts
        // (and the recorder's view lookup) would miss the match.
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
                {
                    continue;
                }
                const uint32_t ri = bind.declared_consume.index;
                if (ri >= resource_count)
                {
                    continue;   // not declared (invalid handle) — relies on markSideEffect
                }

                if (bind.consume_type == ERGResourceType::TEXTURE)
                {
                    bool exists = false;
                    for (const auto& t : pass.textures)
                        if (t.resource.index == ri) { exists = true; break; }
                    if (exists)
                    {
                        continue;   // already declared via .read()/.write()
                    }

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
                    if (exists)
                    {
                        continue;
                    }

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
        if (tds_count == 0)
        {
            return;
        }
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
            if (pi == kNoPass)
            {
                continue;   // unbound DS — nothing to derive a layout from
            }
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
                        {
                            w.image_layout = convertVkImageLayout(determineTextureState(*ref, *tex_desc, pass.type).layout);
                        }
                    }
                }

                // F2: descriptor type ↔ image layout consistency. The layout is
                // derived from the pass role, so a mismatch means the feature declared
                // a role inconsistent with the descriptor type (STORAGE_IMAGE needs
                // GENERAL; sampled/combined/input needs a READ_ONLY layout).
                const bool layout_ok =
                    (w.descriptor_type == EDescriptorType::STORAGE_IMAGE)
                        ? (w.image_layout == EImageLayout::GENERAL)
                    : (w.descriptor_type == EDescriptorType::INPUT_ATTACHMENT)
                        ? (w.image_layout == EImageLayout::RENDERING_LOCAL_READ)
                        : (w.image_layout == EImageLayout::SHADER_READ_ONLY_OPTIMAL ||
                           w.image_layout == EImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL);
                if (!layout_ok)
                    compiled.diagnostics.push_back(
                        renderError<err::graph::TransientDescriptorLayoutMismatch>(
                            ri,
                            static_cast<std::uint32_t>(w.descriptor_type),
                            static_cast<std::uint32_t>(w.image_layout)));
            }
        }
    }

    // 1.6) Validate that every bindResourceDS declares the resource it consumes
    // ---------------------------
    void RenderGraphCompiler::validateResourceDSBindings(RGCompiledGraph& compiled)
    {
        const auto& graph = compiled.original_graph;
        const uint32_t rc = static_cast<uint32_t>(graph.resources.size());

        // SAMPLED 与 transient usage 的交叉校验已在
        // unionTextureUsageFromRoles() 中结构化为 UsageUnderdeclared 或
        // TransientUsageViolation；这里仅检查 descriptor binding 的依赖声明。

        for (std::uint32_t pass_index = 0; pass_index < graph.passes.size(); ++pass_index)
        {
            const auto& pass = graph.passes[pass_index];
            if (pass.has_side_effect)
            {
                continue;   // explicit opt-out (legacy markSideEffect path)
            }

            for (const auto& bind : pass.ds_bindings)
            {
                if (bind.source != EDSBindingSource::Resource)
                {
                    continue;
                }
                if (bind.declared_consume.index < rc)
                {
                    continue;   // declared — the graph sees the consumption
                }

                compiled.diagnostics.push_back(
                    renderError<err::graph::ResourceBindingConsumeMissing>(
                        pass_index,
                        bind.slot));
            }
        }
    }

    // 3.05) Starved Pass Elimination
    // ---------------------------
    //
    // The DUAL of dead-pass elimination below. That one prunes by OUTPUT — a
    // pass whose product reaches nothing exported cannot matter. This one
    // prunes by INPUT — a pass whose source was never produced cannot run.
    //
    // Both are needed and neither substitutes for the other. validateResources
    // used to rely on dead-pass elimination to "prune any reader" of an
    // unresolved OPTIONAL forward reference, but that BFS starts at exported
    // resources and walks BACKWARDS: a reader that also writes something
    // exported is reachable, so it survives with a missing input.
    //
    // SsaoResolve is exactly that shape — it reads an unresolved LinearDepth
    // and writes a PERSISTENT SceneSsao. It therefore stayed in the graph
    // while its input got no barrier at all (computeBarriers skips invalid
    // resources), leaving a descriptor that claims SHADER_READ_ONLY_OPTIMAL
    // pointed at an image still in COLOR_ATTACHMENT_OPTIMAL. Desktop drivers
    // shrug; a tiler hangs. The degrade was half-applied — barrier gone,
    // binding kept — which is worse than either extreme.
    //
    // Propagation is forward and transitive: disabling a pass makes its own
    // outputs unavailable, which can starve its consumers in turn. Iterated to
    // a fixpoint rather than swept once, because a starved consumer becomes
    // visible only after its producer has been marked.
    void RenderGraphCompiler::eliminateStarvedPasses(RGCompiledGraph& compiled)
    {
        auto& graph = compiled.original_graph;
        const uint32_t pass_count     = static_cast<uint32_t>(graph.passes.size());
        const uint32_t resource_count = static_cast<uint32_t>(graph.resources.size());
        if (pass_count == 0)
        {
            return;
        }

        // Seed: UNRESOLVED FORWARD REFERENCES only.
        //
        // NOT `!valid_resources[i]`, which is the trap this function is here to
        // close and would have walked straight into: that flag means "do not
        // emit barriers against this", and EXTERNAL resources carry it too —
        // ext.LightResources, ext.MaterialResources, ext.HzbPyramid and friends
        // are feature-owned and entirely available, they are just not
        // graph-managed. Seeding on the flag would starve nearly every pass in
        // a real scene.
        //
        // A FORWARD_REFERENCE surviving to here genuinely has no backing image:
        // resolveForwardReferences rewrites every ref that DID resolve, so what
        // remains is a name nobody created.
        std::vector<bool> unavailable(resource_count, false);
        for (uint32_t i = 0; i < resource_count; ++i)
            unavailable[i] =
                (graph.resources[i].lifetime == ERGResourceLifetime::FORWARD_REFERENCE);

        // pass_touches, not pass_reads: WRITING an unbacked resource is just as
        // impossible as reading one, and it is the more dangerous half. A write
        // to a texture is an ATTACHMENT — the pass calls vkCmdBeginRendering
        // against an image view that does not exist. That is what actually hung
        // the phone: with SSAO attached, LinearDepthResolve survived dead-pass
        // elimination (SSAO's disabled reader still counted as a consumer in
        // that BFS) and began a render pass on the unbacked LinearDepth. With
        // SSAO absent it was pruned as dead, which is precisely why the hang
        // looked like SSAO's fault for so long.
        std::vector<std::vector<uint32_t>> resource_writers(resource_count);
        std::vector<std::vector<uint32_t>> pass_touches(pass_count);
        for (uint32_t pi = 0; pi < pass_count; ++pi)
        {
            const auto& pass = graph.passes[pi];
            for (const auto& tex : pass.textures)
            {
                if (tex.resource.index >= resource_count)
                {
                    continue;
                }
                if (tex.usage == ERGResourceUsage::WRITE || tex.usage == ERGResourceUsage::READ_WRITE)
                {
                    resource_writers[tex.resource.index].push_back(pi);
                }
                pass_touches[pi].push_back(tex.resource.index);
            }
            for (const auto& buf : pass.buffers)
            {
                if (buf.resource.index >= resource_count)
                {
                    continue;
                }
                if (buf.usage == ERGResourceUsage::WRITE || buf.usage == ERGResourceUsage::READ_WRITE)
                {
                    resource_writers[buf.resource.index].push_back(pi);
                }
                pass_touches[pi].push_back(buf.resource.index);
            }
        }

        std::vector<bool> starved(pass_count, false);
        bool changed = true;
        while (changed)
        {
            changed = false;

            for (uint32_t pi = 0; pi < pass_count; ++pi)
            {
                if (starved[pi] || !graph.passes[pi].enabled)
                {
                    continue;
                }
                for (uint32_t ri : pass_touches[pi])
                {
                    if (!unavailable[ri])
                    {
                        continue;
                    }
                    starved[pi] = true;
                    changed     = true;
                    // Name BOTH the pass and the resource: "which effect went
                    // missing" and "what was missing" are different questions,
                    // and acting on it needs the answer to both.
                    compiled.diagnostics.push_back(
                        renderError<err::graph::PassStarvedOfInput>(pi, ri));
                    break;
                }
            }

            // A resource all of whose writers are starved is itself
            // unavailable. Guard on HAVING writers: an IMPORTED resource has
            // none inside the graph and is perfectly available.
            for (uint32_t ri = 0; ri < resource_count; ++ri)
            {
                if (unavailable[ri] || resource_writers[ri].empty())
                {
                    continue;
                }
                bool all_starved = true;
                for (uint32_t w : resource_writers[ri])
                    if (!starved[w]) { all_starved = false; break; }
                if (all_starved) { unavailable[ri] = true; changed = true; }
            }
        }

        std::vector<uint32_t> new_order;
        new_order.reserve(compiled.execution_order.size());
        for (uint32_t pi : compiled.execution_order)
        {
            if (!starved[pi])
            {
                new_order.push_back(pi);
            }
            else              graph.passes[pi].enabled = false;
        }
        compiled.execution_order = std::move(new_order);

        for (auto& group : compiled.render_pass_layout.groups)
            std::erase_if(group.passes, [&](const RGPassInRenderPass& p) {
                return p.pass_index < pass_count && starved[p.pass_index];
            });
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

        if (pass_count == 0)
        {
            return;
        }

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
                // no-op for them.
                resource_is_exported[i] = true;
            }
            else if (res.lifetime == ERGResourceLifetime::IMPORTED)
            {
                // Imported texture with a required final layout (e.g. swapchain → PRESENT_SRC)
                if (res.import_info && res.import_info->final_layout != VK_IMAGE_LAYOUT_UNDEFINED)
                {
                    resource_is_exported[i] = true;
                }
                // Imported buffer (assumed needed by CPU / external system)
                if (res.import_buffer_info)
                {
                    resource_is_exported[i] = true;
                }
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
                {
                    resource_writers[tex.resource.index].push_back(pi);
                }
                if (tex.usage == ERGResourceUsage::READ  || tex.usage == ERGResourceUsage::READ_WRITE)
                {
                    pass_reads[pi].push_back(tex.resource.index);
                }
            }
            for (const auto& buf : pass.buffers)
            {
                if (buf.usage == ERGResourceUsage::WRITE || buf.usage == ERGResourceUsage::READ_WRITE)
                {
                    resource_writers[buf.resource.index].push_back(pi);
                }
                if (buf.usage == ERGResourceUsage::READ  || buf.usage == ERGResourceUsage::READ_WRITE)
                {
                    pass_reads[pi].push_back(buf.resource.index);
                }
            }
        }

        // Ping-pong closure: the PREVIOUS handle and the CURRENT handle are the
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
                    {
                        resource_writers[i].push_back(w);
                    }
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
            if (!resource_is_exported[ri])
            {
                continue;
            }
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
            if (!cpass.pass)
            {
                continue;
            }

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
    // the moment it sees such a dependency; compile() then aborts.
    //
    // At record time the RGVulkanRecorder creates a single VkSemaphore
    // (timeline type) and uses these values for cross-queue synchronization.

    // H: Derive precise VkPipelineStageFlags2 from resource role + pass type,
    //    replacing the former blanket ALL_COMMANDS_BIT on semaphore sync points.
    static VkPipelineStageFlags2 writeStageForTexture(lux::render::ETextureRole role, ERGPassType pt)
    {
        switch (role) {
        case lux::render::ETextureRole::COLOR_ATTACHMENT:         return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        case lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT: return VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        case lux::render::ETextureRole::UNORDERED_ACCESS:         return shaderStageForPass(pt);
        default: return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
    }

    static VkPipelineStageFlags2 readStageForTexture(lux::render::ETextureRole role, ERGPassType pt)
    {
        switch (role) {
        case lux::render::ETextureRole::COLOR_ATTACHMENT:         return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        case lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT: return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        case lux::render::ETextureRole::SAMPLED:
        case lux::render::ETextureRole::UNORDERED_ACCESS:         return shaderStageForPass(pt);
        default: return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
    }

    static VkPipelineStageFlags2 writeStageForBuffer(ERGBufferRole role, ERGPassType pt)
    {
        if (pt == ERGPassType::TRANSFER || pt == ERGPassType::ASYNC_TRANSFER)
        {
            return VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        }
        switch (role) {
        case ERGBufferRole::STORAGE: return shaderStageForPass(pt);
        default: return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
    }

    static VkPipelineStageFlags2 readStageForBuffer(ERGBufferRole role, ERGPassType pt)
    {
        if (pt == ERGPassType::TRANSFER || pt == ERGPassType::ASYNC_TRANSFER)
        {
            return VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        }
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
        if (!mq.has_async_work)
        {
            return;
        }

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
            if (!desc)
            {
                continue;
            }

            // --- Check reads for cross-queue producer dependency ---
            // H: consumer_stage parameter enables precise semaphore wait masks.
            auto check_cross_queue_read = [&](uint32_t res_idx,
                                              VkPipelineStageFlags2 consumer_stage)
            {
                if (!compiled.compile_error.ok())
                {
                    return; // already rejected — stop analysis
                }
                if (res_idx >= resource_count)
                {
                    return;
                }
                const auto& writer = last_writer[res_idx];
                if (writer.pass_index == UINT32_MAX)
                {
                    return;
                }
                if (writer.queue == cpass.queue_type)
                {
                    return; // same queue — no sync needed
                }

                // fail-fast guard — genuine cross-queue resource dependency.
                // A resource written on one queue and read on another needs a queue-
                // family ownership transfer (QFOT): RG resources are created
                // VK_SHARING_MODE_EXCLUSIVE and the recorder emits every barrier with
                // VK_QUEUE_FAMILY_IGNORED, so without an explicit release-on-producer /
                // acquire-on-consumer ownership handoff the consumer reads UNDEFINED
                // contents. QFOT is not implemented, so rather than silently corrupt
                // data we reject the graph here. The timeline-semaphore sync-point code
                // below is the (currently unreachable) future home for real multi-queue
                // support; enabling it means implementing QFOT and removing this guard.
                compiled.compile_error = renderError<err::graph::CrossQueueTransferRequired>(
                    res_idx,
                    static_cast<std::uint32_t>(writer.queue),
                    static_cast<std::uint32_t>(cpass.queue_type));
                return;

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
                // unconditionally. The cross-queue guard above aborts compilation before any
                // cross-queue resource dependency reaches this point, so this sync-point
                // code is currently unreachable; it remains as the implementation site
                // for true multi-queue support. To enable it: implement QFOT (recorder
                // overrides of src/dstQueueFamilyIndex + release/acquire submit handoff),
                // then remove the guard.
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
                    {
                        w.write_stage |= ws; // same pass, accumulate
                    }
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
                    {
                        w.write_stage |= ws;
                    }
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
        {
            return;
        }

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
            {
                continue;
            }

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
        {
            return;
        }

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
            {
                last_bound_pipeline = VK_NULL_HANDLE;
            }
        }
    }

} // namespace lux::render
