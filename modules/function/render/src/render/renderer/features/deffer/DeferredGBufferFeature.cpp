#include <lux/engine/render/renderer/features/deffer/DeferredGBufferFeature.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/pipeline/ShadingModelRegistry.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>   // set-7 bind
#include <lux/engine/render/pipeline/VertexLayoutRegistry.hpp>  // vertex-layout SSOT
#include <lux/engine/render/pipeline/VertexLayoutSpec.hpp>      // appendVertexLayoutSpecs
#include <lux/engine/render/resources/vertex/VertexProduction.hpp>     // producer registry
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/core/VulkanCheck.hpp>
#include <lux/engine/render/core/MaterialFamily.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <array>
#include <cassert>

namespace lux::render
{

    // =========================================================================
    //  Construction / destruction
    // =========================================================================

    DeferredGBufferFeature::DeferredGBufferFeature(Config cfg)
        : cfg_(std::move(cfg))
    {
    }

    DeferredGBufferFeature::~DeferredGBufferFeature()
    {
        // §2.2: Layout owned by DescriptorService — no manual destroy.
        visible_set_layout_ = VK_NULL_HANDLE;
        destroyCommon();
    }

    // =========================================================================
    //  Lifecycle
    // =========================================================================

    void DeferredGBufferFeature::initAndAttachTo(RenderScene & /*scene*/)
    {
        init();
    }

    void DeferredGBufferFeature::onDetachFromScene(RenderScene & /*scene*/)
    {
        // §2.2: Layout owned by DescriptorService — no manual destroy.
        visible_set_layout_ = VK_NULL_HANDLE;
        destroyCommon();
    }

    // =========================================================================
    //  Initialisation
    // =========================================================================

    void DeferredGBufferFeature::init()
    {
        // ---- Ensure builtin shader defaults ----
        {
            auto *shaders = renderContext().globalRegistry().find<ShaderResources>();
            cfg_.cull_compute_shader            = ensureBuiltinShader(shaders, cfg_.cull_compute_shader,            EBuiltinShader::MESH_CULL_UNIFIED_COMP);
            cfg_.compact_compute_shader         = ensureBuiltinShader(shaders, cfg_.compact_compute_shader,         EBuiltinShader::MDC_COMPACT_COMP);
            cfg_.gbuffer_vertex_shader          = ensureBuiltinShader(shaders, cfg_.gbuffer_vertex_shader,          EBuiltinShader::GBUFFER_VERT);
            cfg_.gbuffer_unlit_fragment_shader   = ensureBuiltinShader(shaders, cfg_.gbuffer_unlit_fragment_shader,   EBuiltinShader::GBUFFER_UNLIT_FRAG);
            cfg_.gbuffer_pbr_fragment_shader     = ensureBuiltinShader(shaders, cfg_.gbuffer_pbr_fragment_shader,     EBuiltinShader::GBUFFER_PBR_FRAG);
            cfg_.gbuffer_stylized_fragment_shader = ensureBuiltinShader(shaders, cfg_.gbuffer_stylized_fragment_shader, EBuiltinShader::GBUFFER_STYLIZED_FRAG);
        }

        // ---- Common infrastructure (instance storage, buffers, cull) ----
        // Shadow rendering is handled separately by MeshShadowFeature.
        initCommon(cfg_.cull_compute_shader, cfg_.extension_flags);

        auto &ctx = renderContext();
        auto *shaders = ctx.globalRegistry().find<ShaderResources>();
        auto &gbuf_vs = *shaders->get(cfg_.gbuffer_vertex_shader);

        auto *fs_unlit    = shaders->get(cfg_.gbuffer_unlit_fragment_shader);
        auto *fs_pbr      = shaders->get(cfg_.gbuffer_pbr_fragment_shader);
        auto *fs_stylized = shaders->get(cfg_.gbuffer_stylized_fragment_shader);
        assert(fs_unlit && "DeferredGBufferFeature: gbuffer_unlit_fragment_shader index is invalid");
        assert(fs_pbr && "DeferredGBufferFeature: gbuffer_pbr_fragment_shader index is invalid");
        assert(fs_stylized && "DeferredGBufferFeature: gbuffer_stylized_fragment_shader index is invalid");
        // Graph family frag is OPTIONAL (no builtin). null => Graph family gets no
        // pipeline (registerFamilyPipelines skips it). No assert.
        auto *fs_graph = cfg_.gbuffer_graph_fragment_shader.is_null()
                             ? nullptr
                             : shaders->get(cfg_.gbuffer_graph_fragment_shader);

        if (visible_set_layout_ == VK_NULL_HANDLE)
        {
            const VkDescriptorSetLayoutBinding visible_binding{
                0,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                1,
                VK_SHADER_STAGE_VERTEX_BIT,
                nullptr,
            };
            auto id = ctx.descriptorService().registerLayout({.bindings = {&visible_binding, 1},
                                                              .debug_name = "GBufferVisibleSetLayout"});
            visible_set_layout_ = ctx.descriptorService().layout(id);
        }

        // ---- GBuffer graphics pipelines (MRT: 3 color + depth, per family) ----
        // A single pipeline per (family, cull_mode) — the _vp shader
        // reads vertices from set 7 for BOTH static (via MeshInputPool) and
        // skinned (via SkinningResources transient pool) draws. Both share
        // the SAME stride (full 22-float layout 0), so one spec set serves
        // all draws. Mirrors ForwardMeshFeature.
        {
            auto base = makeOpaqueMeshTemplate();
            base.descriptor_set_count = 8;
            base.vertex_shader = gbuf_vs.module;
            base.fragment_shader = (fs_unlit != nullptr) ? fs_unlit->module : VK_NULL_HANDLE;
            base.blend_enable = VK_FALSE;
            base.depth_test_enable = VK_TRUE;
            base.depth_write_enable = VK_TRUE;
            base.depth_compare_op = VK_COMPARE_OP_LESS;

            const VkPushConstantRange pc{
                VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
            const std::array set_layouts{
                ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Scene),     // 0
                instance_res_->descriptorSetLayout(),                            // 1
                ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Texture),  // 2
                ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Light),    // 3
                ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Material), // 4
                visible_set_layout_,                                             // 5
                ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Compute),  // 6 placeholder (unused by graphics)
                ctx.descriptorLayouts().getVertexPoolSetLayout(),                // 7 bindless vertex pool
            };
            const std::array pcs{pc};
            base.pipeline_layout = ctx.pipelineLayoutService().getOrCreate(
                {
                    .set_layouts = set_layouts,
                    .push_constants = pcs,
                    .debug_name = "DeferredGBufferDrawLayout"
                }
            ).value();

            // Feed the _vp gbuffer shader the global VBO layout spec.
            // Skinned compute writes the same 22-float layout (bone tail
            // zero-padded) so one spec works for both.
            auto* vlr = ctx.globalRegistry().find<VertexLayoutRegistry>();
            const VertexLayoutId vp_read_layout = kDefaultVertexLayoutId;

            // Family → fragment shader (mirrors ForwardMeshFeature). Adding a
            // shading model in an existing family takes zero changes here; a new
            // lighting-technique family needs a new case below. The pipeline
            // registration itself lives in the shared base method
            // registerFamilyPipelines (same source of truth as the forward path).
            auto* shading_registry =
                ctx.globalRegistry().find<ShadingModelRegistry>();
            assert(shading_registry &&
                   "DeferredGBufferFeature: ShadingModelRegistry missing from global registry");

            auto resolveFragmentShader =
                [&](EShadingModel sm) -> const ShaderObject*
            {
                return resolveFragmentForFamily(sm, fs_unlit, fs_pbr, fs_stylized, fs_graph);
            };

            registerFamilyPipelines(base, &gbuf_vs, vlr, vp_read_layout, *shading_registry, resolveFragmentShader);
        }

        // ---- Compact compute pipeline (per-MDC, replaces finalize for view path) ----
        initCompactPipeline(cfg_.compact_compute_shader, "DeferredGBufferCompactLayout");
    }

    // =========================================================================
    //  Render graph passes
    // =========================================================================

    void DeferredGBufferFeature::addPasses(RGBuilder &builder)
    {
        auto &ctx = renderContext();

        // ---- Create GBuffer transient textures ----
        RGTextureDescription albedo_desc = RGTextureDescription::Relative(1.0f, 1.0f, lux::common::ETextureFormat::RGBA8_UNORM);
        albedo_desc.usage = static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::COLOR_ATTACHMENT) | static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::SAMPLED) | static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::INPUT_ATTACHMENT);
        auto gbuf_albedo = builder.createTexture(cfg_.gbuffer.albedo_metallic, albedo_desc);

        RGTextureDescription normal_desc = RGTextureDescription::Relative(1.0f, 1.0f, lux::common::ETextureFormat::RGBA16_SFLOAT);
        normal_desc.usage = static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::COLOR_ATTACHMENT) | static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::SAMPLED) | static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::INPUT_ATTACHMENT);
        auto gbuf_normal = builder.createTexture(cfg_.gbuffer.normal_roughness, normal_desc);

        auto emissive_fmt = renderScene().pipelineConfig().isLdr()
                                ? lux::common::ETextureFormat::RGBA8_UNORM
                                : lux::common::ETextureFormat::RGBA16_SFLOAT;
        RGTextureDescription emissive_desc = RGTextureDescription::Relative(1.0f, 1.0f, emissive_fmt);
        emissive_desc.usage = static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::COLOR_ATTACHMENT) | static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::SAMPLED) | static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::INPUT_ATTACHMENT);
        auto gbuf_emissive = builder.createTexture(cfg_.gbuffer.emissive_ao, emissive_desc);

        // GBuffer resources are now discoverable by name via builder.referenceTexture().
        // No publish() needed — downstream features reference "GBufAlbedoMetallic" etc. directly.

        // ---- Cull + compact (shared GPU-driven view path; H9 de-dup) ----
        addCullAndCompactPasses(builder, CullCompactParams{
            .prefix                    = "DeferredGBuf",
            .phase                     = ECoreRenderPhase::GBuffer,
            .domain                    = EPassDomain::GBuffer,
            .cull_pass_name            = "DeferredGBufferCull",
            .compact_pass_name         = "DeferredGBufferCompact",
            .descriptor_layout_version = cfg_.descriptor_layout_version,
            .extension_flags           = cfg_.extension_flags,
        });

        auto visible_tds = builder.createTransientDS("DeferredVisibleDS", visible_set_layout_, {
                                                                                                   {0, EDescriptorType::STORAGE_BUFFER, visible_instance_rg_},
                                                                                               });

        auto *mat_res = ctx.globalRegistry().find<MaterialResources>();
        auto variant_buckets = collectVariantBuckets(mat_res);
        // R1: give each graph material its own PSO (gbuffer pass) before the
        // per-bucket pick() loop reads the override.
        registerGraphBucketPipelines(mat_res, /*use_gbuffer=*/true);

        // Per-scene bindless vertex-pool DS (set 7). Mandatory for ALL mesh
        // draws (static + skinned) — both read vertices via set 7.
        auto* vpr = renderScene().sceneRegistry().find<VertexPoolRegistry>();
        const VkDescriptorSet vp_ds = vpr ? vpr->descriptorSet() : VK_NULL_HANDLE;

        // ---- Pass 3: GBuffer draw (graphics, MRT) ----
        auto draw_pass = builder.addPass("DeferredGBufferDraw", ERGPassType::GRAPHICS)
                             .write(gbuf_albedo, lux::common::ETextureRole::COLOR_ATTACHMENT)
                             .write(gbuf_normal, lux::common::ETextureRole::COLOR_ATTACHMENT)
                             .write(gbuf_emissive, lux::common::ETextureRole::COLOR_ATTACHMENT)
                             .write(builder.referenceTexture(cfg_.depth_target), lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
                             .setPipeline(bucket_pipelines_.pick(0u, variant_buckets[0]))
                             .bindSceneDS(0)
                             .bindImmutableDS(1, instance_res_->descriptorSet())
                             .bindImmutableDS(2, ctx.globalRegistry().descriptorSetOf<TextureResources>())
                             .bindResourceDS(4, ctx.globalRegistry().find<MaterialResources>(), &MaterialResources::resolveDS, EDSBindMode::PER_FIF,
                                             builder.trackExternalBuffer("ext.MaterialResources"), ERGResourceType::BUFFER)
                             .bindTransientDS(5, visible_tds)
                             .read(draw_indirect_rg_, ERGBufferRole::INDIRECT)
                             .read(draw_count_rg_, ERGBufferRole::INDIRECT)
                             .read(visible_instance_rg_, ERGBufferRole::STORAGE)
                             .after("DeferredGBufferCompact");

        // One pipeline per VARIANT BUCKET (each resolved by bucket_pipelines_.pick;
        // no skinned half). The kernel's `family_count` is left 0 (below) so it
        // never applies the legacy `+ family_count` skinned offset. (Index 0 set
        // above via setPipeline.)
        const uint32_t bucket_count = static_cast<uint32_t>(variant_buckets.size());
        for (uint32_t b = 1; b < bucket_count; ++b)
            draw_pass.addPipeline(bucket_pipelines_.pick(b, variant_buckets[b]));

        // Bind the bindless vertex pool at set 7 (shared 8-set layout).
        if (vp_ds != VK_NULL_HANDLE)
            draw_pass.bindImmutableDS(7, vp_ds);

        // Order the GBuffer draw after every compute-vertex producer (skinning,
        // future morph/cloth) so the graph inserts the compute→vertex barrier.
        // Iterating the registry keeps deferred in lockstep with forward.
        if (auto* vproducers = renderScene().sceneRegistry().find<VertexProductionRegistry>())
            for (const auto& prod : vproducers->producers())
                draw_pass.read(builder.referenceBuffer(prod.rg_buffer_name), ERGBufferRole::STORAGE);

        // One variant-feature entry per bucket (no skinned half).
        std::vector<uint32_t> variant_features;
        variant_features.reserve(bucket_count);
        for (const auto &bucket : variant_buckets)
            variant_features.push_back(bucket.feature_mask);
        draw_pass.setPipelineVariantFeatures(variant_features);

        draw_pass.setKernel("MeshDraw", 
            makeKernelConfig(
                MeshDrawKernelConfig{
                    .draw_count_rg = draw_count_rg_,
                    .indirect_rg = draw_indirect_rg_,
                    .geometry_mask = supportedGeometryMask(EPassDomain::GBuffer),
                    .mdc_count = mdcCount(),
                    .mdc_entries = instance_res_->mdcTable().entries().data(),
                    // family_count = 0 skips skinned `+N` offset.
                    .family_count = 0u,
                }
            )
        );
    }
} // namespace lux::render
