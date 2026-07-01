#include <lux/engine/render/renderer/features/highlight/HighlightFeature.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>          // MeshDrawKernelConfig / makeKernelConfig
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/pipeline/ShadingModelRegistry.hpp>
#include <lux/engine/render/pipeline/VertexLayoutRegistry.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>
#include <lux/engine/render/resources/vertex/VertexProduction.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/core/VulkanCheck.hpp>
#include <lux/engine/render/core/MaterialFamily.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <array>
#include <cassert>
#include <vector>

namespace lux::render
{
    HighlightFeature::HighlightFeature(Config cfg)
        : cfg_(std::move(cfg))
    {
    }

    HighlightFeature::~HighlightFeature()
    {
        visible_set_layout_  = VK_NULL_HANDLE;   // owned by DescriptorService
        blur_ds_layout_      = VK_NULL_HANDLE;
        composite_ds_layout_ = VK_NULL_HANDLE;
        destroyCommon();
    }

    lux::render::Expected<void> HighlightFeature::initAndAttachTo(RenderScene& /*scene*/){ init();     return {};
    }

    void HighlightFeature::onDetachFromScene(RenderScene& /*scene*/)
    {
        // mask_sampler_ (FifOwned) retires through the deferred-destroy queue. Layouts
        // are owned by DescriptorService.
        visible_set_layout_  = VK_NULL_HANDLE;
        blur_ds_layout_      = VK_NULL_HANDLE;
        composite_ds_layout_ = VK_NULL_HANDLE;
        destroyCommon();
    }

    void HighlightFeature::init()
    {
        auto& ctx     = renderContext();
        VkDevice device = ctx.deviceContext().logicalDevice();
        auto* shaders = ctx.globalRegistry().find<ShaderResources>();

        // ---- Builtin shader defaults ----
        cfg_.cull_shader     = ensureBuiltinShader(shaders, cfg_.cull_shader,     EBuiltinShader::MESH_CULL_UNIFIED_COMP);
        cfg_.compact_shader  = ensureBuiltinShader(shaders, cfg_.compact_shader,  EBuiltinShader::MDC_COMPACT_COMP);
        cfg_.mask_vert       = ensureBuiltinShader(shaders, cfg_.mask_vert,       EBuiltinShader::HIGHLIGHT_MASK_VERT);
        cfg_.mask_frag       = ensureBuiltinShader(shaders, cfg_.mask_frag,       EBuiltinShader::HIGHLIGHT_MASK_FRAG);
        cfg_.blur_frag       = ensureBuiltinShader(shaders, cfg_.blur_frag,       EBuiltinShader::HIGHLIGHT_BLUR_FRAG);
        cfg_.composite_frag  = ensureBuiltinShader(shaders, cfg_.composite_frag,  EBuiltinShader::HIGHLIGHT_COMPOSITE_FRAG);

        // ---- Common GPU-driven infrastructure (instance storage, cull) ----
        initCommon(cfg_.cull_shader, cfg_.extension_flags);

        auto& mask_vs = *shaders->get(cfg_.mask_vert);
        auto& mask_fs = *shaders->get(cfg_.mask_frag);

        // ---- Visible-instance set layout (set 5: cull → draw) ----
        if (visible_set_layout_ == VK_NULL_HANDLE)
        {
            const VkDescriptorSetLayoutBinding b{
                0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
            auto id = ctx.descriptorService().registerLayout(
                {.bindings = {&b, 1}, .debug_name = "HighlightVisibleSetLayout"});
            visible_set_layout_ = ctx.descriptorService().layout(id);
        }

        // ---- Blur input descriptor set layout (set 1: one sampler) ----
        if (blur_ds_layout_ == VK_NULL_HANDLE)
        {
            const VkDescriptorSetLayoutBinding b{
                0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            auto id = ctx.descriptorService().registerLayout(
                {.bindings = {&b, 1}, .debug_name = "HighlightBlurDSLayout"});
            blur_ds_layout_ = ctx.descriptorService().layout(id);
        }

        // ---- Composite descriptor set layout (set 1: blurred + sharp samplers) ----
        if (composite_ds_layout_ == VK_NULL_HANDLE)
        {
            const std::array<VkDescriptorSetLayoutBinding, 2> b{{
                {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            }};
            auto id = ctx.descriptorService().registerLayout(
                {.bindings = {b.data(), b.size()}, .debug_name = "HighlightCompositeDSLayout"});
            composite_ds_layout_ = ctx.descriptorService().layout(id);
        }

        // ---- Mask linear-clamp sampler ----
        {
            VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            si.magFilter = VK_FILTER_LINEAR;  si.minFilter = VK_FILTER_LINEAR;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.maxLod = 0.0f;
            VkSampler s = VK_NULL_HANDLE;
            VK_CHECK(vkCreateSampler(device, &si, nullptr, &s));
            mask_sampler_ = FifOwned<VkSampler>{&ctx.deferredDestroyQueue(), s};
        }

        // ---- Mask DRAW pipeline (per family, but ALL use the same mask frag) ----
        // Reuses the shared 8-set GPU-driven mesh layout + registerFamilyPipelines so
        // the vertex-pool layout spec is handled exactly as Forward/Deferred; the
        // resolver returns the mask frag for every family, so every bucket resolves
        // to the same mask pipeline. Depth-less + no cull → full silhouette (the halo
        // shows even through occlusion, UE-style); single R8 color (format RG-inferred).
        {
            auto base = makeOpaqueMeshTemplate();
            base.descriptor_set_count = 8;
            base.vertex_shader   = mask_vs.module;
            base.fragment_shader = mask_fs.module;
            base.blend_enable        = VK_FALSE;
            base.depth_test_enable   = VK_FALSE;
            base.depth_write_enable  = VK_FALSE;
            base.cull_mode           = VK_CULL_MODE_NONE;

            const VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
            const std::array set_layouts{
                ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Scene),     // 0
                instance_res_->descriptorSetLayout(),                            // 1
                ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Texture),  // 2
                ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Light),    // 3
                ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Material), // 4
                visible_set_layout_,                                             // 5
                ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Compute),  // 6 placeholder
                ctx.descriptorLayouts().getVertexPoolSetLayout(),                // 7 bindless vertex pool
            };
            const std::array pcs{pc};
            base.pipeline_layout = ctx.pipelineLayoutService().getOrCreate(
                {.set_layouts = set_layouts, .push_constants = pcs,
                 .debug_name = "HighlightMaskDrawLayout"}).value();

            auto* vlr = ctx.globalRegistry().find<VertexLayoutRegistry>();
            auto* shading_registry = ctx.globalRegistry().find<ShadingModelRegistry>();
            assert(shading_registry && "HighlightFeature: ShadingModelRegistry missing");

            const ShaderObject* mask_fs_ptr = &mask_fs;
            registerFamilyPipelines(base, &mask_vs, vlr, kDefaultVertexLayoutId,
                                    *shading_registry,
                                    [mask_fs_ptr](EShadingModel) { return mask_fs_ptr; });
        }

        // ---- Fullscreen blur + composite pipelines ----
        // Both reuse the tonemap fullscreen-triangle VS. PC layout follows the Tonemap
        // convention: offset 0..7 = shared scene/view index, custom params start at 8.
        {
            auto* vs_shaders = ctx.globalRegistry().find<ShaderResources>();
            ShaderHandle vert = ensureBuiltinShader(vs_shaders, ShaderHandle{}, EBuiltinShader::TONEMAP_VERT);
            auto& vs  = *vs_shaders->get(vert);
            auto& bfs = *vs_shaders->get(cfg_.blur_frag);
            auto& cfs = *vs_shaders->get(cfg_.composite_frag);

            auto makeFullscreen = [&](const ShaderObject& fs, VkDescriptorSetLayout set1,
                                      bool alpha_blend, uint32_t pc_size, const char* dbg)
                -> GraphicsPipelineHandle
            {
                GraphicsPipelineTemplate tmpl{};
                tmpl.geometry_type        = EGeometryType::MESH;
                tmpl.vertex_shader        = vs.module;
                tmpl.fragment_shader      = fs.module;
                tmpl.descriptor_set_count = 2;
                tmpl.vertex_bindings.clear();
                tmpl.vertex_attributes.clear();
                tmpl.depth_test_enable  = VK_FALSE;
                tmpl.depth_write_enable = VK_FALSE;
                tmpl.cull_mode          = VK_CULL_MODE_NONE;
                if (alpha_blend)
                {
                    // Standard alpha — SRC_ALPHA / ONE_MINUS_SRC_ALPHA (mirrors makeTransparentMeshTemplate).
                    tmpl.blend_enable           = VK_TRUE;
                    tmpl.src_color_blend_factor = VK_BLEND_FACTOR_SRC_ALPHA;
                    tmpl.dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    tmpl.color_blend_op         = VK_BLEND_OP_ADD;
                    tmpl.src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
                    tmpl.dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    tmpl.alpha_blend_op         = VK_BLEND_OP_ADD;
                }
                else
                {
                    tmpl.blend_enable = VK_FALSE;
                }
                tmpl.resource_slot_map.push_back({EDescriptorSetSlot::Scene, 0});

                const VkPushConstantRange pc{
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pc_size};
                const std::array set_layouts{
                    ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Scene),  // set 0
                    set1,                                                          // set 1
                };
                const std::array pcs{pc};
                tmpl.pipeline_layout = ctx.pipelineLayoutService().getOrCreate(
                    {.set_layouts = set_layouts, .push_constants = pcs, .debug_name = dbg}).value();
                // Authoritative PC ranges from the layout — keeps reflection from dropping
                // VERTEX_BIT (the fullscreen VS declares no PC block). Mirrors TonemapFeature.
                tmpl.push_constant_ranges.assign(pcs.begin(), pcs.end());

                std::vector<const rdesc::ShaderInfo*> infos = {&vs.info, &fs.info};
                return ctx.pipelineManager().registerGraphicsTemplate(tmpl, infos).value();
            };

            // Blur: 1 input sampler, no blend; PC = shared(8) + dir_x/dir_y/radius(12) = 20.
            blur_pipeline_      = makeFullscreen(bfs, blur_ds_layout_,      false, 20, "HighlightBlurLayout");
            // Composite: 2 samplers (blurred + sharp), alpha blend; PC = shared(8) + color(12)+intensity(4) = 24.
            composite_pipeline_ = makeFullscreen(cfs, composite_ds_layout_, true,  24, "HighlightCompositeLayout");
        }

        // ---- Compact compute pipeline ----
        initCompactPipeline(cfg_.compact_shader, "HighlightCompactLayout");
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

        // ---- Own frustum cull + compact (same opaque set the gbuffer culls) ----
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
        const VkDescriptorSet vp_ds = vpr ? vpr->descriptorSet() : VK_NULL_HANDLE;

        // ---- Mask draw (depth-less, R8) ----
        auto draw_pass = builder.addPass("HighlightMaskDraw", ERGPassType::GRAPHICS)
            .write(mask_rg, lux::common::ETextureRole::COLOR_ATTACHMENT)
            .setPipeline(bucket_pipelines_.pick(0u, variant_buckets[0]))
            .bindSceneDS(0)
            .bindImmutableDS(1, instance_res_->descriptorSet())
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

        if (vp_ds != VK_NULL_HANDLE)
            draw_pass.bindImmutableDS(7, vp_ds);

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

        draw_pass.setKernel("MeshDraw", makeKernelConfig(MeshDrawKernelConfig{
            .draw_count_rg = draw_count_rg_,
            .indirect_rg   = draw_indirect_rg_,
            .geometry_mask = supportedGeometryMask(EPassDomain::GBuffer),
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

        const float radius = cfg_.glow_radius;

        // Horizontal pass: mask -> blurH.
        auto blur_h_tds = builder.createTransientDS("HighlightBlurH_DS", blur_ds_layout_, {
            {0, EDescriptorType::COMBINED_IMAGE_SAMPLER, mask_rg, mask_sampler_.get(),
             EImageLayout::SHADER_READ_ONLY_OPTIMAL},
        });
        builder.addPass("HighlightBlurH", ERGPassType::GRAPHICS)
            .read(mask_rg, lux::common::ETextureRole::SAMPLED)
            .write(blur_h_rg, lux::common::ETextureRole::COLOR_ATTACHMENT)
            .setPipeline(blur_pipeline_)
            .bindSceneDS(0)
            .bindTransientDS(1, blur_h_tds)
            .setKernelFn([radius](const PassRecordContext& rec) {
                struct { float dx, dy, radius; } pc{1.0f, 0.0f, radius};
                vkCmdPushConstants(rec.cmd, rec.pipeline_layout, rec.pc_stage_flags, 8, sizeof(pc), &pc);
                vkCmdDraw(rec.cmd, 3, 1, 0, 0);
            })
            .setKernel("FullscreenQuad")
            .after("HighlightMaskDraw")
            .stage(ERenderStage::Overlay);

        // Vertical pass: blurH -> blurV (the finished blurred mask).
        auto blur_v_tds = builder.createTransientDS("HighlightBlurV_DS", blur_ds_layout_, {
            {0, EDescriptorType::COMBINED_IMAGE_SAMPLER, blur_h_rg, mask_sampler_.get(),
             EImageLayout::SHADER_READ_ONLY_OPTIMAL},
        });
        builder.addPass("HighlightBlurV", ERGPassType::GRAPHICS)
            .read(blur_h_rg, lux::common::ETextureRole::SAMPLED)
            .write(blur_v_rg, lux::common::ETextureRole::COLOR_ATTACHMENT)
            .setPipeline(blur_pipeline_)
            .bindSceneDS(0)
            .bindTransientDS(1, blur_v_tds)
            .setKernelFn([radius](const PassRecordContext& rec) {
                struct { float dx, dy, radius; } pc{0.0f, 1.0f, radius};
                vkCmdPushConstants(rec.cmd, rec.pipeline_layout, rec.pc_stage_flags, 8, sizeof(pc), &pc);
                vkCmdDraw(rec.cmd, 3, 1, 0, 0);
            })
            .setKernel("FullscreenQuad")
            .after("HighlightBlurH")
            .stage(ERenderStage::Overlay);

        // ---- Composite the OUTER halo over SceneColor (samples blurred + sharp mask) ----
        auto halo_tds = builder.createTransientDS("HighlightHaloDS", composite_ds_layout_, {
            {0, EDescriptorType::COMBINED_IMAGE_SAMPLER, blur_v_rg, mask_sampler_.get(),
             EImageLayout::SHADER_READ_ONLY_OPTIMAL},
            {1, EDescriptorType::COMBINED_IMAGE_SAMPLER, mask_rg, mask_sampler_.get(),
             EImageLayout::SHADER_READ_ONLY_OPTIMAL},
        });

        const float gr = cfg_.glow_color[0];
        const float gg = cfg_.glow_color[1];
        const float gb = cfg_.glow_color[2];
        const float gi = cfg_.glow_intensity;
        builder.addPass("HighlightComposite", ERGPassType::GRAPHICS)
            .read(blur_v_rg, lux::common::ETextureRole::SAMPLED)
            .read(mask_rg, lux::common::ETextureRole::SAMPLED)
            .write(builder.referenceTexture(cfg_.color_target), lux::common::ETextureRole::COLOR_ATTACHMENT)
            .setPipeline(composite_pipeline_)
            .bindSceneDS(0)
            .bindTransientDS(1, halo_tds)
            .setKernelFn([gr, gg, gb, gi](const PassRecordContext& rec) {
                struct { float r, g, b, intensity; } pc{gr, gg, gb, gi};
                vkCmdPushConstants(rec.cmd, rec.pipeline_layout, rec.pc_stage_flags, 8, sizeof(pc), &pc);
                vkCmdDraw(rec.cmd, 3, 1, 0, 0);
            })
            .setKernel("FullscreenQuad")
            .after("HighlightBlurV")
            .stage(ERenderStage::Overlay);
    }

} // namespace lux::render
