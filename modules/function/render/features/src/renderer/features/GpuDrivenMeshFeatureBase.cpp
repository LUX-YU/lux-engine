#include <lux/engine/render/renderer/features/GpuDrivenMeshFeatureBase.hpp>
#include <lux/engine/render/core/FrustumCuller.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/gpu/pipeline/ShaderPermutation.hpp>
#include <lux/engine/render/resources/material/BuiltinShadingModels.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/gpu/pipeline/VertexLayoutRegistry.hpp>
#include <lux/engine/render/gpu/pipeline/VertexLayoutSpec.hpp> // appendVertexLayoutSpecs / kVtxSpecInputBase
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/mesh/MeshCullCandidateSource.hpp>
#include <lux/engine/render/resources/hzb/HzbResources.hpp> // HZB read DS (set 1)
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/mesh/MeshInstanceExtData.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/VulkanCheck.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <unordered_map>
#include <vector>

namespace lux::render
{
    RGResourceHandle GpuDrivenMeshFeatureBase::importInstanceStorageBuffer(
        RGBuilder& builder,
        std::string_view name,
        VkDeviceSize size,
        std::uint32_t stride,
        std::uint32_t element_count,
        InstanceBufferGetter getter
    )
    {
        RGBufferDescription description{};
        description.size = size;
        description.stride = stride;
        description.element_count = element_count;
        description.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
        description.memory_usage = ERGMemoryUsage::GPU_ONLY;
        RGImportedBufferInfo imported{};
        imported.buffer_getter = [this, getter](VkBuffer* output, std::uint32_t capacity) -> std::uint32_t {
            if (output == nullptr || capacity == 0u)
                return 0u;
            output[0] = (instance_res_->*getter)();
            return 1u;
        };
        return builder.importBuffer(name, description, imported);
    }

    // =========================================================================
    //  Per-family pipeline registration (shared by Forward + Deferred)
    // =========================================================================

    Expected<void> GpuDrivenMeshFeatureBase::registerFamilyPipelines(
        const GraphicsPipelineTemplate& base_template,
        ShaderHandle vertex_shader,
        VertexLayoutRegistry& vlr,
        VertexLayoutId vp_read_layout,
        const std::function<ShaderHandle(EShadingModel)>& resolve_fragment
    )
    {
        auto& pm = renderContext().pipelineManager();
        auto& shaders = renderContext().globalRegistry().must<ShaderResources>();

        // 整个 mesh 家族(家族变体 × 剔除模式,加上图材质桶 PSO 的 _vp)的着色器都从
        // 这里取,统一过域合并切换。
        //
        // 录制期描述符集每个 pass 只绑一次,而绘制在变体管线之间切换 —— 一半切了一半
        // 没切必然产生互不兼容的布局。切换入口不再有静默回退:任一 stage 失败即整体
        // 报错,所以「同一 pass 内的变体必须同批切换」这条约束自动成立,不需要先探测
        // 再决定退不退。
        //
        // 所有 stage 一次切完,因此不存在「持有 A 的指针时切换 B」的窗口 —— 这条曾经
        // 靠注释维持的纪律现在由调用形状保证。
        const auto stageKey = [](ShaderHandle h) { return (static_cast<std::uint64_t>(h.index) << 32) | h.gen; };

        std::vector<ShaderHandle> family_stages{vertex_shader};
        std::unordered_map<std::uint64_t, std::size_t> stage_of_source;
        for (const auto& shading : kBuiltinShadingModels)
        {
            const ShaderHandle fs = resolve_fragment(shading.model);
            if (fs.isNull())
                continue;
            // 同一个片元着色器可能被多个 shading model 共用 —— 只切一次。
            if (stage_of_source.try_emplace(stageKey(fs), family_stages.size()).second)
            {
                family_stages.push_back(fs);
            }
        }

        auto switched = shaders.preparePipelineStages(family_stages);
        if (!switched)
            return lux::cxx::unexpected(switched.error());

        // 全部 stage 已在上面切换完毕,这里只按索引取用已经拷贝好的模块与反射。
        auto regVariant = [&](std::size_t fs_stage, VkCullModeFlags cull_mode) -> GraphicsPipelineHandle {
            auto tmpl = base_template;
            tmpl.cull_mode = cull_mode;
            // 一律用切换后的模块,覆盖调用方原先填的那个。
            tmpl.vertex_shader = switched->module(0);
            tmpl.fragment_shader = switched->module(fs_stage);
            // _vp 不消费任何顶点属性 —— 顶点数据经 set 7 读取。
            tmpl.vertex_bindings.clear();
            tmpl.vertex_attributes.clear();
            if (vlr.hasLayout(vp_read_layout))
            {
                appendVertexLayoutSpecs(
                    tmpl.specialization_values,
                    vlr.fetchLayout(vp_read_layout),
                    VK_SHADER_STAGE_VERTEX_BIT,
                    kVtxSpecInputBase
                );
            }

            const std::array<const lux::rdesc::ShaderInfo*, 2> infos{&switched->info(0), &switched->info(fs_stage)};
            auto built = pm.registerGraphicsTemplate(tmpl, infos);
            if (!built)
                return GraphicsPipelineHandle{}; // 缺一个变体好过丢掉整个进程
            return *built;
        };

        // First model registered per family wins the family's bootstrap slot;
        // later models in the same family share it (they differ in feature_mask,
        // not pipeline state). The cull + no-cull arrays are built in lockstep.
        bucket_pipelines_.reset();
        auto& bootstrap_normal = bucket_pipelines_.bootstrap_normal;
        auto& bootstrap_nocull = bucket_pipelines_.bootstrap_nocull;
        for (const auto& shading : kBuiltinShadingModels)
        {
            const auto fi = static_cast<size_t>(shading.family);
            if (fi >= bootstrap_normal.size())
                continue;
            if (bootstrap_normal[fi].valid())
                continue;
            const ShaderHandle fs = resolve_fragment(shading.model);
            if (fs.isNull())
                continue;
            const auto stage = stage_of_source.find(stageKey(fs));
            if (stage == stage_of_source.end())
                continue;
            bootstrap_normal[fi] = regVariant(stage->second, VK_CULL_MODE_BACK_BIT);
            bootstrap_nocull[fi] = regVariant(stage->second, VK_CULL_MODE_NONE);
        }

        // 存下构建输入,好让 addPasses 用图材质运行期提交的片元着色器建它自己的 PSO。
        // 模板与反射存的都是**切换后**的 _vp —— 图材质桶 PSO 必须与家族 bootstrap 共享
        // 同一份布局,否则同一 pass 内的变体互不兼容。
        graph_pso_template_ = std::make_unique<GraphicsPipelineTemplate>(base_template);
        graph_pso_template_->vertex_shader = switched->module(0);
        graph_pso_vp_info_ = switched->info(0); // 拷贝:指针会悬垂
        graph_pso_vlr_ = &vlr;
        graph_pso_vp_layout_ = vp_read_layout;
        graph_pso_cache_.clear();
        return {};
    }

    void GpuDrivenMeshFeatureBase::registerGraphBucketPipelines(MaterialResources* mat_res, bool use_gbuffer)
    {
        if (!mat_res || !graph_pso_template_)
            return;
        auto& shaders = renderContext().globalRegistry().must<ShaderResources>();
        auto& pm = renderContext().pipelineManager();

        const uint32_t bucket_count = mat_res->variantBucketCount();
        for (uint32_t b = 0; b < bucket_count; ++b)
        {
            const auto desc = mat_res->variantBucket(b);
            if (desc.family != ELightingTechnique::Graph)
                continue;
            const ShaderHandle h = use_gbuffer ? desc.graph_gbuffer_shader : desc.graph_forward_shader;
            if (h.isNull())
                continue; // no per-material shader -> stays on the family bootstrap

            // W3a: a single-sided and a double-sided bucket sharing one frag
            // shader need DISTINCT PSOs (different cull mode), so double_sided is
            // part of the cache identity. Exact composite key (no lossy fold). (C10)
            const GraphPsoKey key{h.index, h.gen, desc.graph_double_sided};
            GraphicsPipelineHandle pso{};
            if (auto it = graph_pso_cache_.find(key); it != graph_pso_cache_.end())
            {
                pso = it->second;
            }
            else
            {
                // 运行期提交的图材质片元着色器必须与 bootstrap 走同一套布局,所以它也过
                // 同一个批量切换入口。切换失败(例如资产反射是模块的子集,被门禁拒收)时
                // 跳过这个桶,它就退回用家族 bootstrap 渲染 —— 材质效果暂时缺失,但不会
                // 崩、也不会绑错。缓存键仍然按**源**句柄记。
                auto graph_stage = shaders.preparePipelineStages(std::array{h});
                if (!graph_stage)
                    continue;

                // 与 regVariant 一致:_vp 经 set 7 读顶点,不绑任何顶点属性。
                auto tmpl = *graph_pso_template_;
                // W3a render-state: double_sided -> no back-face cull. (Blend is
                // deferred — there is no transparent mesh pass yet; a Blend graph
                // material renders through this opaque path, see submitGraph note.)
                tmpl.cull_mode = desc.graph_double_sided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
                tmpl.fragment_shader = graph_stage->module(0);
                tmpl.vertex_bindings.clear();
                tmpl.vertex_attributes.clear();
                if (graph_pso_vlr_ && graph_pso_vlr_->hasLayout(graph_pso_vp_layout_))
                {
                    appendVertexLayoutSpecs(
                        tmpl.specialization_values,
                        graph_pso_vlr_->fetchLayout(graph_pso_vp_layout_),
                        VK_SHADER_STAGE_VERTEX_BIT,
                        kVtxSpecInputBase
                    );
                }

                const std::array<const lux::rdesc::ShaderInfo*, 2> infos{&graph_pso_vp_info_, &graph_stage->info(0)};
                auto built = pm.registerGraphicsTemplate(tmpl, infos);
                if (!built)
                    continue;
                pso = built.value();
                graph_pso_cache_.emplace(key, pso);
                // KNOWN LEAK (deferred, 2026-06-13): when an editor material is
                // recompiled it gets a new fragment ShaderHandle => a new `key`
                // here => a new PipelineManager template + VkPipeline variants. The
                // superseded entry is never evicted (PipelineManager has no
                // retirement API), so VkPipelines accumulate across a long material
                // editing session. Bounded by edits-per-session, not a crash; the
                // fix (DeferredDestroyQueue.retirePipeline + a FIF-gated
                // PipelineManager retire + a mark-sweep of stale keys here) was
                // scoped but deliberately deferred.
            }
            // The cull tier matches the bucket's render-state (W3a). The PSO above
            // is already built with the matching cull_mode; register it under the
            // tier the draw-time pick() will request for this bucket.
            bucket_pipelines_.registerBucketPipeline(b, /*two_sided=*/desc.graph_double_sided, pso);
        }
    }

    // =========================================================================
    //  Destruction
    // =========================================================================

    GpuDrivenMeshFeatureBase::~GpuDrivenMeshFeatureBase()
    {
        // Subclass must call destroyCommon() in its own destructor.
    }

    // =========================================================================
    //  Common lifecycle
    // =========================================================================

    void GpuDrivenMeshFeatureBase::onFrameBegin(const FeatureFrameContext& /*ctx*/)
    {
        instance_res_->beginFrame();

        // If the MDC offset LAYOUT changed since the last graph compile, trigger a
        // recompile so the lane plan, buffer sizes and uploaded mdc_info offsets
        // match the current MDC table. We key on layoutSerial (NOT mutationSerial):
        // it bumps only on a capacity-band crossing / a bucket appearing or dying,
        // so steady-state spawn/despawn of existing meshes — which keeps the same
        // buckets within their power-of-two bands — no longer forces a full graph
        // recompile every frame. Offsets and buffer sizes are derived from each
        // MDC's sticky capacity band, so they are unchanged when layoutSerial is.
        // The recompile re-snapshots it in buildMdcOffsets(). (P-7)
        const uint64_t current_serial = instance_res_->mdcTable().layoutSerial();
        if (current_serial != last_compiled_mdc_serial_)
        {
            renderScene().invalidateGraph(EGraphInvalidationReason::MDC_STORAGE_GENERATION);
            last_compiled_mdc_serial_ = current_serial;
        }

        const auto* meshes = renderScene().renderContext().globalRegistry().find<MeshResources>();
        const auto topology_serial = meshes != nullptr ? meshes->iboTopologySerial() : 0u;
        if (topology_serial != last_ibo_topology_serial_)
        {
            renderScene().invalidateGraph(EGraphInvalidationReason::CLASSIC_MESH_SEGMENT_TOPOLOGY);
            last_ibo_topology_serial_ = topology_serial;
        }
    }

    void GpuDrivenMeshFeatureBase::populateFrameContext(RGFrameContext& frame_ctx)
    {
        // §2.6: Use member instead of thread_local, eliminating TLS lookup overhead.
        frame_instance_ext_ = {};
        frame_instance_ext_.slot_count = instance_res_ ? instance_res_->aliveCount() : 0;
        const auto* candidate_source = renderScene().sceneRegistry().find<MeshCullCandidateSource>();
        if (candidate_source != nullptr && candidate_source->active())
        {
            frame_instance_ext_.view_slot_capacity = candidate_source->capacity();
        }
        // Instance cull-enable mask address for THIS frame, read from the scene's
        // DOMAIN-NEUTRAL primitive (0 = none → the cull shader treats every instance
        // as active). A provider feature (e.g. SpatialCullFeature) publishes it in its
        // onFrameBegin, which runs before populateFrameContext. This base feature does
        // NOT know about SpatialCullGrid / large-world — that is the decoupling seam.
        //
        // A candidate source is already the authoritative coarse filter for the view.
        // Applying the legacy distance mask as well would intersect two independently
        // configured residency windows (the old default is only 512 m) and silently
        // reject valid Fine Render / HLOD candidates.  It would also restore a CPU
        // per-frame scan over every alive slot, defeating the two-level cluster path.
        // Scenes without a candidate source retain the legacy opt-in mask behavior.
        frame_instance_ext_.active_mask_addr =
            candidate_source != nullptr && candidate_source->active() ? 0ull : renderScene().instanceCullMaskAddress();
        auto& global = renderContext().globalRegistry();
        if (const auto* material = global.find<MaterialResources>())
        {
            frame_instance_ext_.graph_material_addr = material->graphMaterialAddress(frame_ctx.frame_index);
            frame_instance_ext_.graph_material_capacity = material->graphMaterialCapacity();
        }
        if (const auto* textures = global.find<TextureResources>())
        {
            frame_instance_ext_.wanted_mip_addr = textures->mipFeedbackAddress(frame_ctx.frame_index);
            frame_instance_ext_.texture_slot_capacity = textures->mipFeedbackCapacity();
        }
        const auto isl = meshInstanceExtSlot();
        if (isl != kInvalidExtSlot)
            frame_ctx.ext_data[isl] = &frame_instance_ext_;
    }

    // =========================================================================
    //  Initialisation
    // =========================================================================

    Expected<void>
    GpuDrivenMeshFeatureBase::initCommon(ShaderHandle view_cull_shader_id, GpuDrivenMeshExtFlags extension_flags)
    {
        auto& ctx = renderContext();
        device_ = ctx.deviceContext().logicalDevice();
        vma_ = ctx.vmaAllocator();
        extension_flags_ = extension_flags;
        hzb_mode_spec_ = extension_flags_.containsAll(EGpuDrivenMeshExt::HZB) ? 1u : 0u;
        auto& shaders = ctx.globalRegistry().must<ShaderResources>();

        // ---- Three-stream instance storage (from scene registry, shared) ----
        {
            // InstanceResources is ensure<>d AND init()ed by StandardMeshStack; a
            // GPU-driven mesh feature only find<>s it. If it is absent (feature
            // installed without StandardMeshStack), fail the install instead of
            // dereferencing null.
            instance_res_ = renderScene().sceneRegistry().find<InstanceResources>();
            if (!instance_res_)
                return renderFailure<err::resource::NotFound>();
        }

        // ---- Descriptor set layout (cull) ----
        createCullLayout();

        // ---- View cull compute pipeline ----
        {
            const VkPushConstantRange pc{
                VK_SHADER_STAGE_COMPUTE_BIT,
                0,
                static_cast<uint32_t>(sizeof(MeshCullPushConstants))};
            const std::array pcs{pc};
            std::string debug_name = std::string(name()) + "CullLayout";

            // When HZB is on, the view cull also binds set 1 (HZB combined sampler
            // + view-param UBO). Register the matching layout — the DescriptorService
            // dedups it to the one HzbFeature created. Shadow/compact stay set-0-only
            // (this set 1 is exclusive to the view-cull pipeline layout).
            VkPipelineLayout pl_handle = VK_NULL_HANDLE;
            if (hzb_mode_spec_ != 0u)
            {
                std::array<VkDescriptorSetLayoutBinding, 2> rb{};
                rb[0] = {0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
                rb[1] = {1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
                DescriptorLayoutDesc rd{};
                rd.bindings = std::span<const VkDescriptorSetLayoutBinding>(rb.data(), rb.size());
                rd.debug_name = "HzbReadSet";
                const VkDescriptorSetLayout hzb_read =
                    ctx.descriptorService().layout(ctx.descriptorService().registerLayout(rd));
                const std::array sl{cull_set_layout_, hzb_read};
                auto pl_exp = ctx.pipelineLayoutService().getOrCreate(
                    PipelineLayoutDesc{.set_layouts = sl, .push_constants = pcs, .debug_name = debug_name.c_str()}
                );
                if (!pl_exp) // propagate layout-creation failure, don't .value()-throw
                    return lux::cxx::unexpected(pl_exp.error());
                pl_handle = pl_exp.value();
            }
            else
            {
                const std::array sl{cull_set_layout_};
                auto pl_exp = ctx.pipelineLayoutService().getOrCreate(
                    PipelineLayoutDesc{.set_layouts = sl, .push_constants = pcs, .debug_name = debug_name.c_str()}
                );
                if (!pl_exp) // propagate layout-creation failure, don't .value()-throw
                    return lux::cxx::unexpected(pl_exp.error());
                pl_handle = pl_exp.value();
            }
            const std::array<GraphicsPipelineTemplate::ShaderSpecializationValue, 3> cull_specs{{
                {VK_SHADER_STAGE_COMPUTE_BIT, 0u, 0u},
                {VK_SHADER_STAGE_COMPUTE_BIT, 2u, kGeometryKindCount},
                {VK_SHADER_STAGE_COMPUTE_BIT, kSpecConstHZBMode, hzb_mode_spec_},
            }};
            // shaders / the resolved cull shader may be null (registry misconfig or an
            // unresolved builtin) — guard before dereferencing ->module.
            auto* cull_shader = shaders.get(view_cull_shader_id);
            if (!cull_shader)
                return renderFailure<err::asset::Invalid>();
            view_cull_pipeline_ =
                ctx.pipelineManager().registerComputePipeline(cull_shader->module, pl_handle, cull_specs);
        }
        return {};
    }

    Expected<void>
    GpuDrivenMeshFeatureBase::initCompactPipeline(ShaderHandle compact_shader_id, std::string_view debug_name)
    {
        auto& ctx = renderContext();
        auto& shaders = ctx.globalRegistry().must<ShaderResources>();
        auto* compact_shader = shaders.get(compact_shader_id);
        if (!compact_shader) // unresolved builtin / bad handle from config
            return renderFailure<err::asset::Invalid>();

        const VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        const std::array layouts{cull_set_layout_};
        const std::array pcs{pc};
        const std::string layout_name{debug_name};
        auto pl = ctx.pipelineLayoutService().getOrCreate({
            .set_layouts = layouts,
            .push_constants = pcs,
            .debug_name = layout_name.c_str(),
        }
        );
        if (!pl) // propagate layout-creation failure, don't .value()-throw
            return lux::cxx::unexpected(pl.error());
        compact_pipeline_ = ctx.pipelineManager().registerComputePipeline(compact_shader->module, pl.value(), {});
        return {};
    }

    void GpuDrivenMeshFeatureBase::addCompactPass(
        RGBuilder& builder,
        std::string_view pass_name,
        std::string_view after_pass,
        RGTransientDSHandle cull_tds
    )
    {
        auto compact_pass = builder.addPass(pass_name, ERGPassType::COMPUTE);
        compact_pass.setComputePipeline(compact_pipeline_)
            .bindTransientDS(0, cull_tds)
            .readWrite(draw_count_rg_, ERGBufferRole::STORAGE)
            .readWrite(draw_indirect_rg_, ERGBufferRole::STORAGE)
            .read(mdc_info_rg_, ERGBufferRole::STORAGE)
            .after(after_pass)
            .setKernelFn([this](const PassRecordContext& pctx) {
                if (pctx.pipeline_layout == VK_NULL_HANDLE)
                    return;

                // 录制期 live 读 mdcCount(),而 kernel config 里是编译期快照
                // ——两者一致的前提是 mdc 布局变化(layoutSerial)触发图重编。
                const uint32_t mdc_count = mdcCount();
                vkCmdPushConstants(
                    pctx.cmd,
                    pctx.pipeline_layout,
                    VK_SHADER_STAGE_COMPUTE_BIT,
                    0,
                    sizeof(uint32_t),
                    &mdc_count
                );
                vkCmdDispatch(pctx.cmd, (mdc_count + 63u) / 64u, 1u, 1u);
            }
            )
            .setKernel(
                "MdcCompact",
                makeKernelConfig(MdcCompactKernelConfig{
                    .draw_count_rg = draw_count_rg_,
                    .indirect_rg = draw_indirect_rg_,
                    .mdc_count = mdcCount(),
                })
            );
    }

    void GpuDrivenMeshFeatureBase::destroyCommon() noexcept
    {
        // §2.1: cull_set_layout_ is now owned by DescriptorService — no manual destroy.
        cull_set_layout_ = VK_NULL_HANDLE;

        instance_res_ = nullptr;
        compact_pipeline_ = {};
        extension_flags_ = {};
        hzb_mode_spec_ = 0u;
        vma_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
    }

    // =========================================================================
    //  Descriptor set creation
    // =========================================================================

    void GpuDrivenMeshFeatureBase::createCullLayout()
    {
        // §2.1: Use DescriptorService for layout caching + lifetime management.
        // DescriptorService::registerLayout() deduplicates by bindings content,
        // replacing the previous global ref-counted singleton.
        // Bindings 0-7 are the common mesh inputs; binding 8 maps a dense
        // invocation index to the stable instance slot. World-partition state is
        // still carried independently through the push-constant BDA.
        const std::array<VkDescriptorSetLayoutBinding, 9> bindings{{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        }};

        auto& ds = renderContext().descriptorService();
        const auto layout_id = ds.registerLayout({
            .bindings = bindings,
            .debug_name = std::string(name()) + "_CullLayout",
        }
        );
        cull_set_layout_ = ds.layout(layout_id);
    }

    // =========================================================================
    //  MDC-based buffer sizing
    // =========================================================================

    // =========================================================================
    //  Shared mesh index-buffer imports (all GPU-driven mesh draw paths)
    // =========================================================================
    std::span<const RGResourceHandle> GpuDrivenMeshFeatureBase::importSharedIndexBuffers(RGBuilder& builder)
    {
        imported_index_buffers_rg_.clear();
        auto* mesh_res = renderScene().renderContext().globalRegistry().find<MeshResources>();
        if (mesh_res == nullptr)
            return {};

        RGBufferDescription desc{};
        // size 留 0:存储由 MeshResources 拥有,图既不分配也不据此校验(导入缓冲
        // 只校验 buffer_getter 在场),写一个猜测值反而会骗人。
        desc.stride = sizeof(uint32_t);
        desc.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::INDEX);
        desc.memory_usage = ERGMemoryUsage::GPU_ONLY;

        const auto segment_count = mesh_res->iboSegmentCount();
        imported_index_buffers_rg_.reserve(segment_count);
        for (std::uint16_t segment = 0u; segment < segment_count; ++segment)
        {
            RGImportedBufferInfo imp{};
            imp.buffer_getter = [mesh_res, segment](VkBuffer* out, uint32_t cap) -> uint32_t {
                if (out == nullptr || cap == 0u)
                    return 0u;
                out[0] = mesh_res->indexBuffer(segment);
                return 1u;
            };
            imported_index_buffers_rg_.push_back(
                builder.importBuffer("ClassicMeshIndexBuffer." + std::to_string(segment), desc, imp)
            );
        }
        return imported_index_buffers_rg_;
    }

    // =========================================================================
    //  Shared cull + compact construction (Forward + Deferred view paths).
    //  Parameterized by CullCompactParams; MeshShadow keeps its own variant.
    //  (H9 de-dup — was ~150 verbatim lines in each of Forward/Deferred.)
    // =========================================================================
    void GpuDrivenMeshFeatureBase::addCullAndCompactPasses(RGBuilder& builder, const CullCompactParams& p)
    {
        // ---- Build MDC offsets for this frame ----
        buildMdcOffsets();
        const uint32_t mdc_count = std::max(mdcCount(), 1u);

        const std::string prefix{p.prefix};

        // ---- Create draw indirect/count/visible buffers (MDC-based sizing) ----
        {
            RGBufferDescription desc{};
            desc.size = mdcIndirectBufferSize();
            desc.stride = sizeof(VkDrawIndexedIndirectCommand);
            desc.element_count = mdc_count;
            desc.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::INDIRECT;
            desc.usage |= ERGBufferUsageBits::TRANSFER_DST;
            desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
            draw_indirect_rg_ = builder.createBuffer(prefix + "Indirect", desc);
        }
        {
            RGBufferDescription desc{};
            desc.size = mdcDrawCountBufferSize();
            desc.stride = sizeof(uint32_t);
            desc.element_count = mdc_count;
            desc.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::INDIRECT;
            desc.usage |= ERGBufferUsageBits::TRANSFER_DST;
            desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
            draw_count_rg_ = builder.createBuffer(prefix + "Count", desc);
        }
        {
            RGBufferDescription desc{};
            desc.size = mdcVisibleBufferSize();
            desc.stride = sizeof(GpuVisibleInstance);
            desc.element_count = std::max(instance_res_->mdcTable().totalVisibleCapacity(), 1u);
            desc.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
            desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
            visible_instance_rg_ = builder.createBuffer(prefix + "Visible", desc);
        }

        // ---- Exact-origin view-cull data (RG-managed, GPU_ONLY) ----
        RGResourceHandle frustum_ubo_rg{};
        {
            RGBufferDescription desc{};
            desc.size = kViewFrustumStrideBytes;
            desc.stride = sizeof(Frustum::Plane);
            desc.element_count = static_cast<uint32_t>(kViewFrustumStrideBytes / sizeof(Frustum::Plane));
            desc.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::TRANSFER_DST;
            desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
            frustum_ubo_rg = builder.createBuffer(prefix + "FrustumUBO", desc);
        }

        // ---- Import instance buffers (external, from InstanceResources) ----
        const auto cull_meta_rg = importInstanceStorageBuffer(
            builder,
            prefix + "CullMeta",
            instance_res_->fieldStorageImportBytes(sizeof(InstanceCullMeta)),
            sizeof(InstanceCullMeta),
            instance_res_->slotCount(),
            &InstanceResources::cullMetaBuffer
        );
        const auto property_rg = importInstanceStorageBuffer(
            builder,
            prefix + "Property",
            instance_res_->fieldStorageImportBytes(sizeof(InstanceProperty)),
            sizeof(InstanceProperty),
            instance_res_->slotCount(),
            &InstanceResources::propertyBuffer
        );
        const auto section_table_rg = importInstanceStorageBuffer(
            builder,
            prefix + "SectionTable",
            static_cast<VkDeviceSize>(instance_res_->capacity()) * sizeof(MeshSectionRecord),
            sizeof(MeshSectionRecord),
            instance_res_->capacity(),
            &InstanceResources::meshSectionBuffer
        );

        // ---- Import MdcInfo buffer (MDC offsets + section ids, from MdcTable) ----
        // Always imported: the view cull shader statically accesses binding 7.
        // When mdc_count == 0 the buffer carries a safety sentinel so every
        // instance reads mdc_capacity == 0 and is harmlessly skipped.
        RGResourceHandle mdc_info_rg{};
        {
            const auto& gpu_data = instance_res_->mdcTable().gpuData();
            RGBufferDescription desc{};
            desc.size = static_cast<VkDeviceSize>(gpu_data.size()) * sizeof(uint32_t);
            desc.stride = sizeof(uint32_t);
            desc.element_count = static_cast<uint32_t>(gpu_data.size());
            desc.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
            desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
            RGImportedBufferInfo imp{};
            // Capture the ring slot chosen by buildMdcOffsets() for THIS compile
            // (already ran at the top of addCullAndCompactPasses). A later
            // recompile writes a different slot, so frames still recorded against
            // this graph keep reading their own slot — no overwrite under the GPU.
            const uint32_t mdc_info_slot = instance_res_->currentMdcInfoSlot();
            imp.buffer_getter = [this, mdc_info_slot](VkBuffer* out, uint32_t cap) -> uint32_t {
                if (out == nullptr || cap == 0)
                    return 0u;
                out[0] = instance_res_->mdcInfoBufferAt(mdc_info_slot);
                return 1u;
            };
            mdc_info_rg = builder.importBuffer(prefix + "MdcInfo", desc, imp);
            mdc_info_rg_ = mdc_info_rg;
        }

        RGResourceHandle alive_slots_rg{};
        RGResourceHandle candidate_dispatch_rg{};
        const auto* candidate_source = renderScene().sceneRegistry().find<MeshCullCandidateSource>();
        const bool use_coarse_candidates = candidate_source && candidate_source->active();
        if (use_coarse_candidates)
        {
            alive_slots_rg = builder.referenceBuffer(MeshCullCandidateSource::kCandidateSlotsResource);
            // Dispatch the bounded candidate capacity directly. The producer
            // pre-fills unused slots with UINT_MAX and the cull shader rejects
            // them before touching instance storage. Work therefore remains
            // tied to the active-window capacity (never total World size), while
            // draw correctness no longer depends on a second GPU-written command
            // stream. Its count/group words remain fence-retired telemetry.
            candidate_dispatch_rg = {};
        }
        else
        {
            const uint32_t alive_count = instance_res_->aliveCount();
            const auto safe_alive_count = std::max(alive_count, 1u);
            alive_slots_rg = importInstanceStorageBuffer(
                builder,
                prefix + "AliveSlots",
                static_cast<VkDeviceSize>(safe_alive_count) * sizeof(uint32_t),
                sizeof(uint32_t),
                safe_alive_count,
                &InstanceResources::aliveSlotBuffer
            );
        }

        // The world-partition active mask is no longer a descriptor binding: its GPU
        // address (or 0 when large-world is disabled) rides the cull push-constant and
        // is read via buffer_reference in the shader. See populateFrameContext().

        // ---- Transient DS for cull pass (per-view, per-frame) ----
        std::vector<RGDescriptorWrite> cull_bindings = {
            {0, EDescriptorType::STORAGE_BUFFER, frustum_ubo_rg},
            {1, EDescriptorType::STORAGE_BUFFER, cull_meta_rg},
            {2, EDescriptorType::STORAGE_BUFFER, draw_count_rg_},
            {3, EDescriptorType::STORAGE_BUFFER, draw_indirect_rg_},
            {4, EDescriptorType::STORAGE_BUFFER, section_table_rg},
            {5, EDescriptorType::STORAGE_BUFFER, property_rg},
            {6, EDescriptorType::STORAGE_BUFFER, visible_instance_rg_},
            {7, EDescriptorType::STORAGE_BUFFER, mdc_info_rg},
            {8, EDescriptorType::STORAGE_BUFFER, alive_slots_rg},
        };
        auto cull_tds = builder.createTransientDS(prefix + "CullDS", cull_set_layout_, cull_bindings);

        // ---- Pass 1: view cull (compute) ----
        auto cull_pass = builder.addPass(p.cull_pass_name, ERGPassType::COMPUTE);
        cull_pass.setComputePipeline(view_cull_pipeline_).bindTransientDS(0, cull_tds);
        // HZB read DS (set 1) — only when HZB is on, matching the 2-set view-cull
        // pipeline layout. resolveHzbReadDS returns the PREVIOUS ping-pong slot
        // (last frame's pyramid). Absent HzbResources → keep everything (no cull).
        if (hzb_mode_spec_ != 0u)
        {
            auto* hzb = renderScene().sceneRegistry().find<HzbResources>();
            if (hzb != nullptr)
                cull_pass.bindResourceDS(
                    1,
                    hzb,
                    &HzbResources::resolveHzbReadDS,
                    EDSBindMode::PER_FIF,
                    builder.trackExternalTexture("ext.HzbPyramid"),
                    ERGResourceType::TEXTURE
                );
        }
        cull_pass.write(draw_indirect_rg_, ERGBufferRole::STORAGE)
            .write(draw_count_rg_, ERGBufferRole::STORAGE)
            .write(visible_instance_rg_, ERGBufferRole::STORAGE)
            .write(frustum_ubo_rg, ERGBufferRole::STORAGE)
            .read(cull_meta_rg, ERGBufferRole::STORAGE)
            .read(section_table_rg, ERGBufferRole::STORAGE)
            .read(property_rg, ERGBufferRole::STORAGE)
            .read(alive_slots_rg, ERGBufferRole::STORAGE)
            .setKernel(
                "MeshCull",
                makeKernelConfig(MeshCullKernelConfig{
                    .frustum_ubo_rg = frustum_ubo_rg,
                    .draw_count_rg = draw_count_rg_,
                    .indirect_rg = draw_indirect_rg_,
                    .dispatch_indirect_rg = candidate_dispatch_rg,
                    .dispatch_indirect_offset = MeshCullCandidateSource::kDispatchArgsOffset,
                    .pass_mask = static_cast<uint32_t>(passMaskForPhase(p.phase)),
                    .geometry_mask = supportedGeometryMask(),
                    .draw_list_count = mdc_count,
                    .descriptor_layout_version = p.descriptor_layout_version,
                    .extension_flags = p.extension_flags.bits(), // GPU push constant: raw word
                    .mdc_count = mdcCount(),
                })
            );
        if (candidate_dispatch_rg)
        {
            cull_pass.read(candidate_dispatch_rg, ERGBufferRole::INDIRECT);
        }
        if (use_coarse_candidates)
        {
            // Candidate buffers are forward-referenced because the coarse
            // producer is a separate contribution and can declare its pass
            // after this mesh feature.  The resource names alone cannot infer
            // RAW direction in that declaration order.
            cull_pass.after(MeshCullCandidateSource::kProducerPass);
        }

        // ---- Pass 2: MDC compact ----
        addCompactPass(builder, p.compact_pass_name, p.cull_pass_name, cull_tds);
    }

    void GpuDrivenMeshFeatureBase::buildMdcOffsets()
    {
        instance_res_->uploadMdcInfo();
        // Snapshot the LAYOUT serial the offsets were just built from (buildOffsets
        // does not mutate it). onFrameBegin compares against it next frame. (P-7)
        last_compiled_mdc_serial_ = instance_res_->mdcTable().layoutSerial();
    }

    uint32_t GpuDrivenMeshFeatureBase::mdcCount() const noexcept
    {
        return instance_res_->mdcTable().count();
    }

    VkDeviceSize GpuDrivenMeshFeatureBase::mdcIndirectBufferSize() const noexcept
    {
        const uint32_t n = std::max(mdcCount(), 1u);
        return static_cast<VkDeviceSize>(n) * sizeof(VkDrawIndexedIndirectCommand);
    }

    VkDeviceSize GpuDrivenMeshFeatureBase::mdcDrawCountBufferSize() const noexcept
    {
        const uint32_t n = std::max(mdcCount(), 1u);
        return static_cast<VkDeviceSize>(n) * sizeof(uint32_t);
    }

    VkDeviceSize GpuDrivenMeshFeatureBase::mdcVisibleBufferSize() const noexcept
    {
        const uint32_t cap = instance_res_->mdcTable().totalVisibleCapacity();
        return static_cast<VkDeviceSize>(std::max(cap, 1u)) * sizeof(GpuVisibleInstance);
    }

    // (mdcInfoBufferSize 已删:零调用点,且它只是
    //  instance_res_->mdcTable().gpuDataSizeBytes() 的一层转发。)

    PassMask GpuDrivenMeshFeatureBase::passMaskForPhase(ECoreRenderPhase phase) noexcept
    {
        switch (phase)
        {
        case ECoreRenderPhase::Depth:
            return static_cast<PassMask>(eDepthPrepass);
        case ECoreRenderPhase::GBuffer:
            return static_cast<PassMask>(eGBuffer);
        case ECoreRenderPhase::ForwardOpaque:
            return static_cast<PassMask>(eBasePass);
        case ECoreRenderPhase::ForwardTrans:
            return static_cast<PassMask>(eTransparent);
        case ECoreRenderPhase::Shadow:
            return static_cast<PassMask>(eShadow);
        default:
            return 0u;
        }
    }

    uint32_t GpuDrivenMeshFeatureBase::supportedGeometryMask() const noexcept
    {
        // Mesh features also accept skinned meshes so
        // the cull shader buckets them into a SkinnedMesh draw list. Features
        // that registered skinned pipeline variants (ForwardMeshFeature) draw
        // them with the _vp pipeline; those that didn't yet fall back to the
        // static variant (bind pose) — harmless, no crash.
        return (1u << static_cast<uint32_t>(EGeometryKind::StaticMesh)) |
               (1u << static_cast<uint32_t>(EGeometryKind::SkinnedMesh));
    }

} // namespace lux::render
