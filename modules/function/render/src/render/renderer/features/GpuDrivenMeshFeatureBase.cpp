#include <lux/engine/render/renderer/features/GpuDrivenMeshFeatureBase.hpp>
#include <lux/engine/render/renderer/FrustumCuller.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/pipeline/ShaderPermutation.hpp>
#include <lux/engine/render/pipeline/ShadingModelRegistry.hpp>
#include <lux/engine/render/pipeline/VertexLayoutRegistry.hpp>
#include <lux/engine/render/pipeline/VertexLayoutSpec.hpp>     // appendVertexLayoutSpecs / kVtxSpecInputBase
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/hzb/HzbResources.hpp>   // HZB read DS (set 1)
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/graph/MeshInstanceExtData.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/core/VulkanCheck.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace lux::render
{
    namespace
    {
        constexpr uint32_t kGpuDrivenMeshExtFlagHZB = 1u << 0;
    }

    // =========================================================================
    //  Per-family pipeline registration (shared by Forward + Deferred)
    // =========================================================================

    void GpuDrivenMeshFeatureBase::registerFamilyPipelines(
        const GraphicsPipelineTemplate&                              base_template,
        const ShaderObject*                                          vertex_shader,
        VertexLayoutRegistry*                                        vlr,
        VertexLayoutId                                               vp_read_layout,
        ShadingModelRegistry&                                        shading_registry,
        const std::function<const ShaderObject*(EShadingModel)>&     resolve_fragment)
    {
        auto& pm = renderContext().pipelineManager();

        // One pipeline per (family, cull-mode). The _vp vertex shader is shared
        // (set in base_template); only the fragment shader and cull mode differ.
        auto regVariant = [&](const ShaderObject* fs,
                              VkCullModeFlags cull_mode) -> GraphicsPipelineHandle
        {
            assert(fs && "registerFamilyPipelines: family fragment shader is null");
            if (!fs)
                return GraphicsPipelineHandle{};

            auto tmpl = base_template;
            tmpl.cull_mode       = cull_mode;
            tmpl.fragment_shader = fs->module;
            // _vp consumes NO vertex attributes — reads via set 7.
            tmpl.vertex_bindings.clear();
            tmpl.vertex_attributes.clear();
            if (vlr && vlr->hasLayout(vp_read_layout))
                appendVertexLayoutSpecs(tmpl.specialization_values,
                                        vlr->fetchLayout(vp_read_layout),
                                        VK_SHADER_STAGE_VERTEX_BIT, kVtxSpecInputBase);

            std::vector<const lux::rdesc::ShaderInfo*> infos = { &vertex_shader->info, &fs->info };
            return pm.registerGraphicsTemplate(tmpl, infos).value();
        };

        // First model registered per family wins the family's bootstrap slot;
        // later models in the same family share it (they differ in feature_mask,
        // not pipeline state). The cull + no-cull arrays are built in lockstep.
        bucket_pipelines_.reset();
        auto& bootstrap_normal = bucket_pipelines_.bootstrap_normal;
        auto& bootstrap_nocull = bucket_pipelines_.bootstrap_nocull;
        shading_registry.forEachModel(
            [&](EShadingModel sm, const ShadingModelDescriptor& desc)
        {
            const auto fi = static_cast<size_t>(desc.family);
            if (fi >= bootstrap_normal.size()) return;
            if (bootstrap_normal[fi].valid()) return;
            const auto* fs = resolve_fragment(sm);
            if (!fs) return;
            bootstrap_normal[fi] = regVariant(fs, VK_CULL_MODE_BACK_BIT);
            bootstrap_nocull[fi] = regVariant(fs, VK_CULL_MODE_NONE);
        });

        // R1: capture the build inputs so addPasses can construct a graph
        // material's OWN pipeline from its runtime-submitted frag shader.
        graph_pso_template_  = std::make_unique<GraphicsPipelineTemplate>(base_template);
        graph_pso_vp_info_   = vertex_shader->info;   // copy: the pointer would dangle (see hpp)
        graph_pso_vlr_       = vlr;
        graph_pso_vp_layout_ = vp_read_layout;
        graph_pso_cache_.clear();
    }

    void GpuDrivenMeshFeatureBase::registerGraphBucketPipelines(
        MaterialResources* mat_res, bool use_gbuffer)
    {
        if (!mat_res || !graph_pso_template_)
            return;
        auto* shaders = renderContext().globalRegistry().find<ShaderResources>();
        if (!shaders)
            return;
        auto& pm = renderContext().pipelineManager();

        const uint32_t bucket_count = mat_res->variantBucketCount();
        for (uint32_t b = 0; b < bucket_count; ++b)
        {
            const auto desc = mat_res->variantBucket(b);
            if (desc.family != ELightingTechnique::Graph)
                continue;
            const ShaderHandle h = use_gbuffer ? desc.graph_gbuffer_shader
                                               : desc.graph_forward_shader;
            if (h.is_null())
                continue;  // no per-material shader -> stays on the family bootstrap

            // W3a: a single-sided and a double-sided bucket sharing one frag
            // shader need DISTINCT PSOs (different cull mode), so double_sided is
            // part of the cache identity. Exact composite key (no lossy fold). (C10)
            const GraphPsoKey key{ h.index, h.gen, desc.graph_double_sided };
            GraphicsPipelineHandle pso{};
            if (auto it = graph_pso_cache_.find(key); it != graph_pso_cache_.end())
            {
                pso = it->second;
            }
            else
            {
                const ShaderObject* fs = shaders->get(h);
                if (!fs)
                    continue;
                // Mirror registerFamilyPipelines' regVariant: _vp reads vertices
                // via set 7, so no vertex attributes are bound.
                auto tmpl = *graph_pso_template_;
                // W3a render-state: double_sided -> no back-face cull. (Blend is
                // deferred — there is no transparent mesh pass yet; a Blend graph
                // material renders through this opaque path, see submitGraph note.)
                tmpl.cull_mode       = desc.graph_double_sided ? VK_CULL_MODE_NONE
                                                               : VK_CULL_MODE_BACK_BIT;
                tmpl.fragment_shader = fs->module;
                tmpl.vertex_bindings.clear();
                tmpl.vertex_attributes.clear();
                if (graph_pso_vlr_ && graph_pso_vlr_->hasLayout(graph_pso_vp_layout_))
                    appendVertexLayoutSpecs(tmpl.specialization_values,
                                            graph_pso_vlr_->fetchLayout(graph_pso_vp_layout_),
                                            VK_SHADER_STAGE_VERTEX_BIT, kVtxSpecInputBase);
                std::vector<const lux::rdesc::ShaderInfo*> infos = {
                    &graph_pso_vp_info_, &fs->info };
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
            bucket_pipelines_.registerBucketPipeline(
                b, /*two_sided=*/desc.graph_double_sided, pso);
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

    void GpuDrivenMeshFeatureBase::onFrameBegin(const FeatureFrameContext & /*ctx*/)
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
            renderScene().invalidateGraph();
            last_compiled_mdc_serial_ = current_serial;
        }
    }

    void GpuDrivenMeshFeatureBase::populateFrameContext(RGFrameContext& frame_ctx)
    {
        // §2.6: Use member instead of thread_local, eliminating TLS lookup overhead.
        frame_instance_ext_ = {};
        frame_instance_ext_.slot_count = instance_res_ ? instance_res_->slotCount() : 0;
        // Instance cull-enable mask address for THIS frame, read from the scene's
        // DOMAIN-NEUTRAL primitive (0 = none → the cull shader treats every instance
        // as active). A provider feature (e.g. SpatialCullFeature) publishes it in its
        // onFrameBegin, which runs before populateFrameContext. This base feature does
        // NOT know about SpatialCullGrid / large-world — that is the decoupling seam.
        frame_instance_ext_.active_mask_addr = renderScene().instanceCullMaskAddress();
        const auto isl = meshInstanceExtSlot();
        if (isl != kInvalidExtSlot)
            frame_ctx.ext_data[isl] = &frame_instance_ext_;
    }

    // =========================================================================
    //  Initialisation
    // =========================================================================

    void GpuDrivenMeshFeatureBase::initCommon(
        ShaderHandle view_cull_shader_id,
        uint32_t extension_flags)
    {
        auto &ctx = renderContext();
        device_ = ctx.deviceContext().logicalDevice();
        vma_ = ctx.vmaAllocator();
        extension_flags_ = extension_flags;
        hzb_mode_spec_ = ((extension_flags_ & kGpuDrivenMeshExtFlagHZB) != 0u) ? 1u : 0u;
        auto *shaders = ctx.globalRegistry().find<ShaderResources>();

        constexpr uint32_t kMaxInstances = 8192;

        // ---- Three-stream instance storage (from scene registry, shared) ----
        {
            instance_res_ = renderScene().sceneRegistry().find<InstanceResources>();
            InstanceResources::InitInfo si{};
            si.device_context = &ctx.deviceContext();
            si.descriptor_svc = &ctx.descriptorService();
            si.arena = &renderScene().descriptorArena();
            si.initial_capacity = kMaxInstances;
            si.max_capacity = kMaxInstances; // auto-lift in ensureCapacity(); VRAM is the real limit
            instance_res_->init(si); // idempotent
        }

        // ---- Descriptor set layout (cull) ----
        createCullLayout();

        // ---- View cull compute pipeline ----
        {
            const VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0,
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
                rb[1] = {1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
                DescriptorLayoutDesc rd{};
                rd.bindings   = std::span<const VkDescriptorSetLayoutBinding>(rb.data(), rb.size());
                rd.debug_name = "HzbReadSet";
                const VkDescriptorSetLayout hzb_read = ctx.descriptorService().layout(
                    ctx.descriptorService().registerLayout(rd));
                const std::array sl{cull_set_layout_, hzb_read};
                pl_handle = ctx.pipelineLayoutService().getOrCreate({.set_layouts = sl,
                                                                     .push_constants = pcs,
                                                                     .debug_name = debug_name.c_str()}).value();
            }
            else
            {
                const std::array sl{cull_set_layout_};
                pl_handle = ctx.pipelineLayoutService().getOrCreate({.set_layouts = sl,
                                                                     .push_constants = pcs,
                                                                     .debug_name = debug_name.c_str()}).value();
            }
            const std::array<GraphicsPipelineTemplate::ShaderSpecializationValue, 3> cull_specs{{
                {VK_SHADER_STAGE_COMPUTE_BIT, 0u, 0u},
                {VK_SHADER_STAGE_COMPUTE_BIT, 2u, kGeometryKindCount},
                {VK_SHADER_STAGE_COMPUTE_BIT, kSpecConstHZBMode, hzb_mode_spec_},
            }};
            view_cull_pipeline_ = ctx.pipelineManager().registerComputePipeline(
                shaders->get(view_cull_shader_id)->module,
                pl_handle,
                cull_specs);
        }
    }

    void GpuDrivenMeshFeatureBase::initCompactPipeline(
        ShaderHandle compact_shader_id,
        std::string_view debug_name)
    {
        auto& ctx = renderContext();
        auto* shaders = ctx.globalRegistry().find<ShaderResources>();
        auto* compact_shader = (shaders != nullptr) ? shaders->get(compact_shader_id) : nullptr;
        if (!compact_shader)
        {
            assert(compact_shader && "GpuDrivenMeshFeatureBase: compact shader index is invalid");
            return;
        }

        const VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        const std::array layouts{cull_set_layout_};
        const std::array pcs{pc};
        const std::string layout_name{debug_name};
        auto pl = ctx.pipelineLayoutService().getOrCreate({
            .set_layouts = layouts,
            .push_constants = pcs,
            .debug_name = layout_name.c_str(),
        });
        compact_pipeline_ = ctx.pipelineManager().registerComputePipeline(
            compact_shader->module,
            pl.value(),
            {});
    }

    void GpuDrivenMeshFeatureBase::addCompactPass(
        RGBuilder& builder,
        std::string_view pass_name,
        std::string_view after_pass,
        RGTransientDSHandle cull_tds)
    {
        builder.addPass(pass_name, ERGPassType::COMPUTE)
            .setComputePipeline(compact_pipeline_)
            .bindTransientDS(0, cull_tds)
            .readWrite(draw_count_rg_, ERGBufferRole::STORAGE)
            .readWrite(draw_indirect_rg_, ERGBufferRole::STORAGE)
            .read(mdc_info_rg_, ERGBufferRole::STORAGE)
            .after(after_pass)
            .setKernelFn([this](const PassRecordContext& pctx)
            {
                if (pctx.pipeline_layout == VK_NULL_HANDLE)
                    return;

                const uint32_t mdc_count = mdcCount();
                vkCmdPushConstants(
                    pctx.cmd,
                    pctx.pipeline_layout,
                    VK_SHADER_STAGE_COMPUTE_BIT,
                    0,
                    sizeof(uint32_t),
                    &mdc_count);
                vkCmdDispatch(pctx.cmd, (mdc_count + 63u) / 64u, 1u, 1u);
            })
            .setKernel("MdcCompact", makeKernelConfig(MdcCompactKernelConfig{
                .draw_count_rg = draw_count_rg_,
                .indirect_rg   = draw_indirect_rg_,
                .mdc_count     = mdcCount(),
            }));
    }

    void GpuDrivenMeshFeatureBase::destroyCommon() noexcept
    {
        // §2.1: cull_set_layout_ is now owned by DescriptorService — no manual destroy.
        cull_set_layout_ = VK_NULL_HANDLE;

        instance_res_ = nullptr;
        compact_pipeline_ = {};
        extension_flags_ = 0u;
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
        // 8 general mesh-cull bindings (0-7). The world-partition active mask used to
        // sit at binding 8; it is now passed by buffer-device-address in the cull
        // push-constant (data-driven, optional) so this layout carries no large-world
        // concept and a non-large-world scene needs no extra binding.
        const std::array<VkDescriptorSetLayoutBinding, 8> bindings{{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        }};

        auto& ds = renderContext().descriptorService();
        const auto layout_id = ds.registerLayout({
            .bindings   = bindings,
            .debug_name = std::string(name()) + "_CullLayout",
        });
        cull_set_layout_ = ds.layout(layout_id);
    }

    // =========================================================================
    //  MDC-based buffer sizing
    // =========================================================================

    // =========================================================================
    //  Shared cull + compact construction (Forward + Deferred view paths).
    //  Parameterized by CullCompactParams; MeshShadow keeps its own variant.
    //  (H9 de-dup — was ~150 verbatim lines in each of Forward/Deferred.)
    // =========================================================================
    void GpuDrivenMeshFeatureBase::addCullAndCompactPasses(
        RGBuilder& builder, const CullCompactParams& p)
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
            desc.stride = sizeof(uint32_t);
            desc.element_count = std::max(instance_res_->mdcTable().totalVisibleCapacity(), 1u);
            desc.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
            desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
            visible_instance_rg_ = builder.createBuffer(prefix + "Visible", desc);
        }

        // ---- Frustum UBO (RG-managed, GPU_ONLY, written via vkCmdUpdateBuffer) ----
        RGResourceHandle frustum_ubo_rg{};
        {
            RGBufferDescription desc{};
            desc.size = 6 * sizeof(Frustum::Plane);
            desc.stride = sizeof(Frustum::Plane);
            desc.element_count = 6;
            desc.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::TRANSFER_DST;
            desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
            frustum_ubo_rg = builder.createBuffer(prefix + "FrustumUBO", desc);
        }

        // ---- Import instance buffers (external, from InstanceResources) ----
        RGResourceHandle cull_meta_rg{};
        {
            RGBufferDescription desc{};
            desc.size = static_cast<VkDeviceSize>(instance_res_->slotCount()) * sizeof(InstanceCullMeta);
            desc.stride = sizeof(InstanceCullMeta);
            desc.element_count = instance_res_->slotCount();
            desc.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
            desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
            RGImportedBufferInfo imp{};
            imp.buffer_getter = [this](VkBuffer *out, uint32_t cap) -> uint32_t
            {
                if (out == nullptr || cap == 0) return 0u;
                out[0] = instance_res_->cullMetaBuffer();
                return 1u;
            };
            cull_meta_rg = builder.importBuffer(prefix + "CullMeta", desc, imp);
        }

        RGResourceHandle property_rg{};
        {
            RGBufferDescription desc{};
            desc.size = static_cast<VkDeviceSize>(instance_res_->slotCount()) * sizeof(InstanceProperty);
            desc.stride = sizeof(InstanceProperty);
            desc.element_count = instance_res_->slotCount();
            desc.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
            desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
            RGImportedBufferInfo imp{};
            imp.buffer_getter = [this](VkBuffer *out, uint32_t cap) -> uint32_t
            {
                if (out == nullptr || cap == 0) return 0u;
                out[0] = instance_res_->propertyBuffer();
                return 1u;
            };
            property_rg = builder.importBuffer(prefix + "Property", desc, imp);
        }

        RGResourceHandle section_table_rg{};
        {
            RGBufferDescription desc{};
            desc.size = static_cast<VkDeviceSize>(instance_res_->capacity()) * sizeof(MeshSectionRecord);
            desc.stride = sizeof(MeshSectionRecord);
            desc.element_count = instance_res_->capacity();
            desc.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
            desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
            RGImportedBufferInfo imp{};
            imp.buffer_getter = [this](VkBuffer *out, uint32_t cap) -> uint32_t
            {
                if (out == nullptr || cap == 0) return 0u;
                out[0] = instance_res_->meshSectionBuffer();
                return 1u;
            };
            section_table_rg = builder.importBuffer(prefix + "SectionTable", desc, imp);
        }

        // ---- Import MdcInfo buffer (MDC offsets + section ids, from MdcTable) ----
        // Always imported: the view cull shader statically accesses binding 7.
        // When mdc_count == 0 the buffer carries a safety sentinel so every
        // instance reads mdc_capacity == 0 and is harmlessly skipped.
        RGResourceHandle mdc_info_rg{};
        {
            const auto& gpu_data = instance_res_->mdcTable().gpuData();
            RGBufferDescription desc{};
            desc.size          = static_cast<VkDeviceSize>(gpu_data.size()) * sizeof(uint32_t);
            desc.stride        = sizeof(uint32_t);
            desc.element_count = static_cast<uint32_t>(gpu_data.size());
            desc.usage         = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
            desc.memory_usage  = ERGMemoryUsage::GPU_ONLY;
            RGImportedBufferInfo imp{};
            // Capture the ring slot chosen by buildMdcOffsets() for THIS compile
            // (already ran at the top of addCullAndCompactPasses). A later
            // recompile writes a different slot, so frames still recorded against
            // this graph keep reading their own slot — no overwrite under the GPU.
            const uint32_t mdc_info_slot = instance_res_->currentMdcInfoSlot();
            imp.buffer_getter = [this, mdc_info_slot](VkBuffer *out, uint32_t cap) -> uint32_t
            {
                if (out == nullptr || cap == 0) return 0u;
                out[0] = instance_res_->mdcInfoBufferAt(mdc_info_slot);
                return 1u;
            };
            mdc_info_rg = builder.importBuffer(prefix + "MdcInfo", desc, imp);
            mdc_info_rg_ = mdc_info_rg;
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
        };
        auto cull_tds = builder.createTransientDS(prefix + "CullDS", cull_set_layout_, cull_bindings);

        // ---- Pass 1: view cull (compute) ----
        auto cull_pass = builder.addPass(p.cull_pass_name, ERGPassType::COMPUTE);
        cull_pass.setComputePipeline(view_cull_pipeline_)
                 .bindTransientDS(0, cull_tds);
        // HZB read DS (set 1) — only when HZB is on, matching the 2-set view-cull
        // pipeline layout. resolveHzbReadDS returns the PREVIOUS ping-pong slot
        // (last frame's pyramid). Absent HzbResources → keep everything (no cull).
        if (hzb_mode_spec_ != 0u)
        {
            auto* hzb = renderScene().sceneRegistry().find<HzbResources>();
            if (hzb != nullptr)
                cull_pass.bindResourceDS(1, hzb, &HzbResources::resolveHzbReadDS, EDSBindMode::PER_FIF,
                                         builder.trackExternalTexture("ext.HzbPyramid"), ERGResourceType::TEXTURE);
        }
        cull_pass
            .write(draw_indirect_rg_, ERGBufferRole::STORAGE)
            .write(draw_count_rg_, ERGBufferRole::STORAGE)
            .write(visible_instance_rg_, ERGBufferRole::STORAGE)
            .write(frustum_ubo_rg, ERGBufferRole::STORAGE)
            .read(cull_meta_rg, ERGBufferRole::STORAGE)
            .read(section_table_rg, ERGBufferRole::STORAGE)
            .read(property_rg, ERGBufferRole::STORAGE)
            .setKernel("MeshCull", makeKernelConfig(MeshCullKernelConfig{
                .frustum_ubo_rg = frustum_ubo_rg,
                .draw_count_rg  = draw_count_rg_,
                .indirect_rg    = draw_indirect_rg_,
                .pass_mask      = static_cast<uint32_t>(passMaskForPhase(p.phase)),
                .geometry_mask  = supportedGeometryMask(p.domain),
                .draw_list_count = mdc_count,
                .descriptor_layout_version = p.descriptor_layout_version,
                .extension_flags = p.extension_flags,
                .mdc_count      = mdcCount(),
            }));

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
        return static_cast<VkDeviceSize>(std::max(cap, 1u)) * sizeof(uint32_t);
    }

    VkDeviceSize GpuDrivenMeshFeatureBase::mdcInfoBufferSize() const noexcept
    {
        return static_cast<VkDeviceSize>(instance_res_->mdcTable().gpuDataSizeBytes());
    }

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

    uint32_t GpuDrivenMeshFeatureBase::supportedGeometryMask(EPassDomain pass_domain) const noexcept
    {
        (void)pass_domain;
        // Mesh features also accept skinned meshes so
        // the cull shader buckets them into a SkinnedMesh draw list. Features
        // that registered skinned pipeline variants (ForwardMeshFeature) draw
        // them with the _vp pipeline; those that didn't yet fall back to the
        // static variant (bind pose) — harmless, no crash.
        return (1u << static_cast<uint32_t>(EGeometryKind::StaticMesh))
             | (1u << static_cast<uint32_t>(EGeometryKind::SkinnedMesh));
    }

} // namespace lux::render
