#include <lux/engine/render/renderer/features/highlight/HighlightFeature.hpp>
#include <lux/engine/function/render/client/genops/HighlightOperation.ops.hpp>

// 生成物(构建树 pass_gen/,由 HighlightBlurPassParams.hpp /
// HighlightCompositePassParams.hpp 的注解生成):PC 布局 + 图 I/O +
// 瞬态 DS + 推送常量,与发射进着色器的声明同源。
#include <HighlightBlurPassParams.pass.hpp>
#include <HighlightCompositePassParams.pass.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>          // MeshDrawKernelConfig / makeKernelConfig
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/gpu/pipeline/VertexLayoutRegistry.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>
#include <lux/engine/render/resources/vertex/VertexProduction.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/VulkanCheck.hpp>
#include <lux/engine/render/resources/material/MaterialFamily.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <array>
#include <bit>
#include <cassert>
#include <cstdio>
#include <vector>

namespace lux::render
{
    HighlightFeature::HighlightFeature(Config cfg)
        : cfg_(std::move(cfg))
    {
    }

    HighlightFeature::~HighlightFeature()
    {
        releaseAll();
    }

    void HighlightFeature::releaseAll() noexcept
    {
        // Layouts owned by DescriptorService; mask_sampler_ 是其采样器缓存的
        // 共享句柄,同样不归本特性销毁。
        visible_set_layout_  = VK_NULL_HANDLE;
        blur_ds_layout_      = VK_NULL_HANDLE;
        composite_ds_layout_ = VK_NULL_HANDLE;
        destroyCommon();
    }

    lux::render::Expected<void> HighlightFeature::initAndAttachTo(RenderScene& /*scene*/){ return init(); }

    void HighlightFeature::onDetachFromScene(RenderScene& /*scene*/)
    {
        releaseAll();
    }

    Expected<void> HighlightFeature::init()
    {
        auto& ctx     = renderContext();
        VkDevice device = ctx.deviceContext().logicalDevice();
        auto& shaders = ctx.globalRegistry().must<ShaderResources>();

        // 空句柄回填内置默认。
        const std::array backfill{
            ShaderStageSlot{EBuiltinShader::MESH_CULL_UNIFIED_COMP,   &cfg_.cull_shader},
            ShaderStageSlot{EBuiltinShader::MDC_COMPACT_COMP,         &cfg_.compact_shader},
            ShaderStageSlot{EBuiltinShader::HIGHLIGHT_MASK_VERT,      &cfg_.mask_vert},
            ShaderStageSlot{EBuiltinShader::HIGHLIGHT_MASK_FRAG,      &cfg_.mask_frag},
            ShaderStageSlot{EBuiltinShader::HIGHLIGHT_BLUR_FRAG,      &cfg_.blur_frag},
            ShaderStageSlot{EBuiltinShader::HIGHLIGHT_COMPOSITE_FRAG, &cfg_.composite_frag}};
        if (auto filled = resolveShaderStages(shaders, backfill); !filled)
            return filled;

        // ---- Common GPU-driven infrastructure (instance storage, cull) ----
        // Propagate a hard prerequisite failure so the feature isn't registered as
        // installed (never expected — the editor installs Highlight with
        // StandardMeshStack). (audit feat-init)
        if (auto r = initCommon(cfg_.cull_shader, cfg_.extension_flags); !r)
            return r;

        assert(shaders.get(cfg_.mask_vert) && "HighlightFeature: mask_vert is invalid");
        assert(shaders.get(cfg_.mask_frag) && "HighlightFeature: mask_frag is invalid");

        // ---- Visible-instance set layout (set 5: cull → draw) ----
        if (visible_set_layout_ == VK_NULL_HANDLE)
        {
            auto id = ctx.descriptorService().registerLayout(
                storageBufferVertexLayout("HighlightVisibleSetLayout"));
            visible_set_layout_ = ctx.descriptorService().layout(id);
        }

        // (The set1 layout for blur/composite is already built via the reflected
        //  layout path — see makeFullscreen below: after registration it is
        //  retrieved via templateSetLayout for transient DS allocation.)

        // ---- 遮罩采样器:共享缓存(线性 clamp)----
        mask_sampler_ = ctx.descriptorService().sampler(SamplerDesc::linearClamp());

        // ---- Mask DRAW pipeline (per family, but ALL use the same mask frag) ----
        // Reuses the shared 8-set GPU-driven mesh layout + registerFamilyPipelines so
        // the vertex-pool layout spec is handled exactly as Forward/Deferred; the
        // resolver returns the mask frag for every family, so every bucket resolves
        // to the same mask pipeline. Depth-less + no cull → full silhouette (the halo
        // shows even through occlusion, UE-style); single R8 color (format RG-inferred).
        {
            auto base = makeOpaqueMeshTemplate();
            base.descriptor_set_count = 8;
            // The vertex/fragment modules are filled in internally by
            // registerFamilyPipelines, uniformly using the switched-over
            // (centralized) handles, so they aren't pre-filled here.
            base.blend_enable        = VK_FALSE;
            base.depth_test_enable   = VK_FALSE;
            base.depth_write_enable  = VK_FALSE;
            base.cull_mode           = VK_CULL_MODE_NONE;

            // Reflected layout (same as ForwardMesh / DeferredGBuffer).
            base.debug_name = "HighlightMaskDraw";
            base.push_constant_ranges.push_back({VK_SHADER_STAGE_VERTEX_BIT, 0, kViewPushPrefixSize});

            auto& vlr = ctx.globalRegistry().must<VertexLayoutRegistry>();
            const ShaderHandle mask_frag = cfg_.mask_frag;
            if (auto family = registerFamilyPipelines(
                    base,
                    cfg_.mask_vert,
                    vlr,
                    kDefaultVertexLayoutId,
                    [mask_frag](EShadingModel) { return mask_frag; }
                );
                !family)
                return family;
        }

        // ---- Fullscreen blur + composite pipelines ----
        // Both reuse the tonemap fullscreen-triangle VS. PC layout follows the Tonemap
        // convention: offset 0..7 = shared scene/view index, custom params start at 8.
        {
            auto& vs_shaders = ctx.globalRegistry().must<ShaderResources>();

            // 三个 stage 一次解析 + 切换:全屏三角顶点 + 模糊片元 + 合成片元。
            // blur/composite 只有一个私有 set1 加一个 Scene 空洞,切换实质是空操作,
            // 但标记本身是注册通过的前提。
            const std::array fullscreen_requests{
                PipelineStageRequest{EBuiltinShader::TONEMAP_VERT, {}},
                PipelineStageRequest{EBuiltinShader::HIGHLIGHT_BLUR_FRAG,      cfg_.blur_frag},
                PipelineStageRequest{EBuiltinShader::HIGHLIGHT_COMPOSITE_FRAG, cfg_.composite_frag}};

            auto fullscreen = preparePipelineStages(vs_shaders, fullscreen_requests);
            if (!fullscreen)
                return lux::cxx::unexpected(fullscreen.error());

            // 布局留空,由反射加契约建出来:set1(uBlurSrc / uBlur+uMask)来自反射,
            // set0 = Scene 靠 resource_slot_map 的槽位语义补上。注册之后
            // templateSetLayout 取出 set1 布局,供 transient DS 分配。
            constexpr std::size_t kVertexStage = 0;
            auto makeFullscreen =
                [&](std::size_t frag_stage, VkDescriptorSetLayout& out_set1_layout,
                    bool alpha_blend, uint32_t pc_size, const char* dbg)
                -> Expected<GraphicsPipelineHandle>
            {
                GraphicsPipelineTemplate tmpl = makeFullscreenTemplate(dbg, pc_size, alpha_blend);
                tmpl.vertex_shader   = fullscreen->module(kVertexStage);
                tmpl.fragment_shader = fullscreen->module(frag_stage);

                const std::array<const rdesc::ShaderInfo*, 2> infos{
                    &fullscreen->info(kVertexStage), &fullscreen->info(frag_stage)};
                auto handle = ctx.pipelineManager().registerGraphicsTemplate(tmpl, infos);
                if (!handle)
                    return lux::cxx::unexpected(handle.error());

                out_set1_layout = ctx.pipelineManager().templateSetLayout(*handle, 1);
                if (out_set1_layout == VK_NULL_HANDLE)
                    return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(1);
                return *handle;
            };

            // PC 尺寸走与 GLSL 同源的生成常量,不手写字面量。
            auto blur = makeFullscreen(1, blur_ds_layout_, false,
                pass_gen::kHighlightBlurPassParamsPCTotalSize, "HighlightBlur");
            if (!blur)
                return lux::cxx::unexpected(blur.error());
            blur_pipeline_ = *blur;

            auto composite = makeFullscreen(2, composite_ds_layout_, true,
                pass_gen::kHighlightCompositePassParamsPCTotalSize, "HighlightComposite");
            if (!composite)
                return lux::cxx::unexpected(composite.error());
            composite_pipeline_ = *composite;
        }

        // ---- Compact compute pipeline ----
        if (auto r = initCompactPipeline(cfg_.compact_shader, "HighlightCompactLayout"); !r)
            return lux::cxx::unexpected(r.error());   // propagate, don't swallow
        return {};
    }

    // =========================================================================
    //  Render graph passes
    // =========================================================================
    void HighlightFeature::addPasses(RGBuilder& builder)
    {
        auto& ctx = renderContext();

        // ---- R8 highlight mask (transient; format RG-inferred at bake) ----
        RGTextureDescription mask_desc =
            RGTextureDescription::Relative(1.0f, 1.0f, lux::common::ETextureFormat::R8_UNORM);
        mask_desc.usage = static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::COLOR_ATTACHMENT)
                        | static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::SAMPLED);
        auto mask_rg = builder.createTexture(cfg_.mask_target, mask_desc);

        // ---- 整链跳过条件:无任何存活实例带 highlight 位(编辑器无选中是常态)时,
        // cull/compact/mask/blur×2/composite 六个 pass 作为一条 condition chain
        // 原子跳过,链内 transient 每帧 UNDEFINED 起手,布局链自洽
        // (CONDITIONAL_CHAIN,classifyElectivePasses)。
        //
        // 作用域即链的边界:它存续期间 builder.addPass 出来的每个 pass 自动入链。
        // 此前是六处各写一遍 .setCondition(cond, tag) —— 链的范围要数遍那六处调用
        // 才知道,漏挂一个只会在编译期被兜底捕获。
        auto chain = builder.conditionChain([this]() -> bool {
            constexpr uint32_t bit = std::countr_zero(kInstanceFlagHighlight);
            return instance_res_ != nullptr && instance_res_->flagBitCount(bit) > 0u;
        });

        // ---- Own frustum cull + compact (same opaque set the gbuffer culls) ----
        // 这两个 pass 由共享辅助函数建,同样经 builder.addPass 出来,因此自动入链。
        addCullAndCompactPasses(builder, CullCompactParams{
            .prefix                    = "Hl",
            .phase                     = ECoreRenderPhase::GBuffer,
            .domain                    = EPassDomain::GBuffer,
            .cull_pass_name            = "HighlightCull",
            .compact_pass_name         = "HighlightCompact",
            .descriptor_layout_version = cfg_.descriptor_layout_version,
            .extension_flags           = cfg_.extension_flags,
        });

        auto visible_tds = builder.createTransientDS("HighlightVisibleDS", visible_set_layout_, {
            {0, EDescriptorType::STORAGE_BUFFER, visible_instance_rg_},
        });

        auto* mat_res = ctx.globalRegistry().find<MaterialResources>();
        auto variant_buckets = collectVariantBuckets(mat_res);

        auto* vpr = renderScene().sceneRegistry().find<VertexPoolRegistry>();

        // ---- Mask draw (depth-less, R8) ----
        auto draw_pass = builder.addPass("HighlightMaskDraw", ERGPassType::GRAPHICS)
            .write(mask_rg, lux::common::ETextureRole::COLOR_ATTACHMENT)
            .setPipeline(bucket_pipelines_.pick(0u, variant_buckets[0]))
            .bindSceneDS()
            .useEngineSet(EDescriptorSetSlot::Instance)
            // Mask shaders use ONLY sets 0/1/5/7 (view / instance / visible / vertex-pool). Unlike the
            // gbuffer draw they sample no textures and read no per-material SSBO, so sets 2 (Texture)
            // and 4 (Material) are intentionally left UNBOUND — which also keeps the spurious
            // ext.MaterialResources per-frame dependency out of the graph. The 8-set pipeline layout
            // is unchanged (shared GPU-driven mesh layout); sets the shaders never access need no bind.
            .bindTransientDS(5, visible_tds)
            .read(draw_indirect_rg_, ERGBufferRole::INDIRECT)
            .read(draw_count_rg_, ERGBufferRole::INDIRECT)
            .read(visible_instance_rg_, ERGBufferRole::STORAGE)
            .after("HighlightCompact")
            .stage(ERenderStage::Overlay);   // ordered before the composite via the mask read/write dep

        const uint32_t bucket_count = static_cast<uint32_t>(variant_buckets.size());
        for (uint32_t b = 1; b < bucket_count; ++b)
            draw_pass.addPipeline(bucket_pipelines_.pick(b, variant_buckets[b]));

        if (vpr && vpr->isInitialized())
            draw_pass.useEngineSet(EDescriptorSetSlot::VertexPool);

        // Order after skinning (live skeletal silhouette).
        if (auto* vproducers = renderScene().sceneRegistry().find<VertexProductionRegistry>())
            for (const auto& prod : vproducers->producers())
                draw_pass.read(builder.referenceBuffer(prod.rg_buffer_name), ERGBufferRole::STORAGE);

        {
            std::vector<uint32_t> vf;
            vf.reserve(bucket_count);
            for (const auto& bucket : variant_buckets)
                vf.push_back(bucket.feature_mask);
            draw_pass.setPipelineVariantFeatures(vf);
        }

        const auto index_buffers = importSharedIndexBuffers(builder);
        draw_pass.setKernel("MeshDraw", makeKernelConfig(MeshDrawKernelConfig{
            .draw_count_rg = draw_count_rg_,
            .indirect_rg   = draw_indirect_rg_,
            .index_buffers_rg = index_buffers.data(),
            .index_buffer_count = static_cast<std::uint32_t>(
                index_buffers.size()),
            .geometry_mask = supportedGeometryMask(),
            .mdc_count     = mdcCount(),
            .mdc_entries   = instance_res_->mdcTable().entries().data(),
            .family_count  = 0u,
        }));

        // ---- Separable Gaussian blur of the mask (H then V) into two R8 transients ----
        RGTextureDescription blur_desc =
            RGTextureDescription::Relative(1.0f, 1.0f, lux::common::ETextureFormat::R8_UNORM);
        blur_desc.usage = static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::COLOR_ATTACHMENT)
                        | static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::SAMPLED);
        auto blur_h_rg = builder.createTexture("HighlightBlurH", blur_desc);
        auto blur_v_rg = builder.createTexture("HighlightBlurV", blur_desc);

        // ---- 以下三个 pass 各消费一份 PassParams(生成物):描述符写、
        // 图 I/O、推送常量、PC 尺寸全部同源自注解头,原先的匿名 PC 结构 ×3、
        // 手写 DS 写 ×3、尺寸字面量 ×2 就是它们杀死的重复。 ----

        // Horizontal pass: mask -> blurH.
        const HighlightBlurPassParams blur_h_params{
            .blur_src         = mask_rg,
            .blur_src_sampler = mask_sampler_,
            .color_out        = blur_h_rg,
            .scalars          = {.dir_x = 1.0f, .dir_y = 0.0f, .radius = cfg_.glow_radius}};
        auto blur_h_tds = pass_gen::createTransientDS(builder, blur_ds_layout_, blur_h_params);
        auto blur_h_pass = builder.addPass("HighlightBlurH", ERGPassType::GRAPHICS);
        pass_gen::declareGraphIO(blur_h_pass, blur_h_params);
        blur_h_pass
            .setPipeline(blur_pipeline_)
            .bindSceneDS()
            .bindTransientDS(1, blur_h_tds)
            .setKernelFn([s = blur_h_params.scalars](const PassRecordContext& rec) {
                pass_gen::pushScalars(rec, s);
                vkCmdDraw(rec.cmd, 3, 1, 0, 0);
            })
            .setKernel("FullscreenQuad")
            .after("HighlightMaskDraw")
            .stage(ERenderStage::Overlay);

        // Vertical pass: blurH -> blurV (the finished blurred mask).
        const HighlightBlurPassParams blur_v_params{
            .blur_src         = blur_h_rg,
            .blur_src_sampler = mask_sampler_,
            .color_out        = blur_v_rg,
            .scalars          = {.dir_x = 0.0f, .dir_y = 1.0f, .radius = cfg_.glow_radius}};
        auto blur_v_tds = pass_gen::createTransientDS(builder, blur_ds_layout_, blur_v_params);
        auto blur_v_pass = builder.addPass("HighlightBlurV", ERGPassType::GRAPHICS);
        pass_gen::declareGraphIO(blur_v_pass, blur_v_params);
        blur_v_pass
            .setPipeline(blur_pipeline_)
            .bindSceneDS()
            .bindTransientDS(1, blur_v_tds)
            .setKernelFn([s = blur_v_params.scalars](const PassRecordContext& rec) {
                pass_gen::pushScalars(rec, s);
                vkCmdDraw(rec.cmd, 3, 1, 0, 0);
            })
            .setKernel("FullscreenQuad")
            .after("HighlightBlurH")
            .stage(ERenderStage::Overlay);

        // ---- Composite the OUTER halo over SceneColor (samples blurred + sharp mask) ----
        const HighlightCompositePassParams halo_params{
            .blur         = blur_v_rg,
            .blur_sampler = mask_sampler_,
            .mask         = mask_rg,
            .mask_sampler = mask_sampler_,
            .color_out    = builder.referenceTexture(cfg_.color_target),
            .scalars      = {.color_r   = cfg_.glow_color[0],
                             .color_g   = cfg_.glow_color[1],
                             .color_b   = cfg_.glow_color[2],
                             .intensity = cfg_.glow_intensity}};
        auto halo_tds = pass_gen::createTransientDS(builder, composite_ds_layout_, halo_params);
        auto halo_pass = builder.addPass("HighlightComposite", ERGPassType::GRAPHICS);
        pass_gen::declareGraphIO(halo_pass, halo_params);
        halo_pass
            .setPipeline(composite_pipeline_)
            .bindSceneDS()
            .bindTransientDS(1, halo_tds)
            .setKernelFn([s = halo_params.scalars](const PassRecordContext& rec) {
                pass_gen::pushScalars(rec, s);
                vkCmdDraw(rec.cmd, 3, 1, 0, 0);
            })
            .setKernel("FullscreenQuad")
            .after("HighlightBlurV")
            .stage(ERenderStage::Overlay);
    }

} // namespace lux::render
