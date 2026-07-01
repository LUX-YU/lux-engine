#include <lux/engine/render/renderer/features/forward/ForwardMeshFeature.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/resources/lighting/LightResources.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>
#include <lux/engine/render/pipeline/ShadingModelRegistry.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>   // set-7 bind
#include <lux/engine/render/resources/vertex/VertexProduction.hpp>     // producer registry
#include <lux/engine/render/pipeline/VertexLayoutRegistry.hpp>  // vertex-layout SSOT
#include <lux/engine/render/pipeline/VertexLayoutSpec.hpp>      // appendVertexLayoutSpecs
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/core/VulkanCheck.hpp>
#include <lux/engine/render/core/MaterialFamily.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <array>
#include <cassert>
#include <iostream>

namespace lux::render
{

    // =========================================================================
    //  Construction / destruction
    // =========================================================================

    ForwardMeshFeature::ForwardMeshFeature(Config cfg)
        : cfg_(std::move(cfg))
    {
    }

    ForwardMeshFeature::~ForwardMeshFeature()
    {
        // §2.2: Layout owned by DescriptorService — no manual destroy.
        visible_set_layout_ = VK_NULL_HANDLE;
        destroyCommon();
    }

    // =========================================================================
    //  Lifecycle
    // =========================================================================

    lux::render::Expected<void> ForwardMeshFeature::initAndAttachTo(RenderScene &scene){
        init();
        return {};
    }

    // =========================================================================
    //  Initialisation
    // =========================================================================

    void ForwardMeshFeature::init()
    {
        // ---- Ensure builtin shader defaults ----
        {
            auto *shaders = renderContext().globalRegistry().find<ShaderResources>();
            cfg_.forward_cull_shader   = ensureBuiltinShader(shaders, cfg_.forward_cull_shader,   EBuiltinShader::MESH_CULL_UNIFIED_COMP);
            cfg_.forward_compact_shader = ensureBuiltinShader(shaders, cfg_.forward_compact_shader, EBuiltinShader::MDC_COMPACT_COMP);
            cfg_.forward_vert_shader   = ensureBuiltinShader(shaders, cfg_.forward_vert_shader,   EBuiltinShader::FORWARD_MESH_VERT);
            cfg_.unlit_fragment        = ensureBuiltinShader(shaders, cfg_.unlit_fragment,        EBuiltinShader::FORWARD_UNLIT_FRAG);
            // C1: default to PCF variant; C2 will let the active ShadowTechnique pick.
            cfg_.pbr_fragment          = ensureBuiltinShader(shaders, cfg_.pbr_fragment,          EBuiltinShader::FORWARD_PBR_FRAG_PCF);
            cfg_.stylized_fragment     = ensureBuiltinShader(shaders, cfg_.stylized_fragment,     EBuiltinShader::FORWARD_STYLIZED_FRAG_PCF);
        }

        // ---- Common infrastructure (instance storage, buffers, cull) ----
        initCommon(cfg_.forward_cull_shader, cfg_.extension_flags);

        auto &ctx = renderContext();
        auto *shaders = ctx.globalRegistry().find<ShaderResources>();
        auto &fwd_vs = *shaders->get(cfg_.forward_vert_shader);

        auto* fs_unlit = shaders->get(cfg_.unlit_fragment);
        auto* fs_pbr = shaders->get(cfg_.pbr_fragment);
        auto* fs_stylized = shaders->get(cfg_.stylized_fragment);
        assert(fs_unlit && "ForwardMeshFeature: unlit_fragment shader index is invalid");
        assert(fs_pbr && "ForwardMeshFeature: pbr_fragment shader index is invalid");
        assert(fs_stylized && "ForwardMeshFeature: stylized_fragment shader index is invalid");
        // Graph family frag is OPTIONAL (no builtin). null => Graph family gets no
        // forward pipeline (registerFamilyPipelines skips it). No assert.
        auto* fs_graph = cfg_.graph_fragment.is_null()
                             ? nullptr
                             : shaders->get(cfg_.graph_fragment);

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
                                                              .debug_name = "ForwardVisibleSetLayout"});
            visible_set_layout_ = ctx.descriptorService().layout(id);
        }

        // ---- Forward graphics pipelines (per material family bucket) ----
        // A single pipeline per (family, cull_mode) — the _vp shader
        // reads vertices from set 7 for BOTH static and skinned draws. The
        // global VBO is registered via MeshInputPool (per scene); skinned
        // outputs go through SkinningResources's transient pool. Both share
        // the SAME stride (full 22-float layout 0), so one spec set serves
        // all draws.
        {
            auto base = makeOpaqueMeshTemplate();
            base.descriptor_set_count = 8;
            base.vertex_shader = fwd_vs.module;
            base.fragment_shader = (fs_unlit != nullptr) ? fs_unlit->module : VK_NULL_HANDLE;

            // The offset-0 8-byte header (instance/material indices) is read by
            // BOTH the vertex (transform) and fragment (material) stages, and
            // the recorder pushes it with the template's reflected stage set
            // (VERTEX|FRAGMENT). The layout's range must therefore include
            // FRAGMENT too, else vkCmdPushConstants trips
            // VUID-vkCmdPushConstants-offset-01795.
            const VkPushConstantRange pc{
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
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
            base.pipeline_layout =
                ctx.pipelineLayoutService().
                getOrCreate({.set_layouts = set_layouts,
                    .push_constants = pcs,
                    .debug_name = "ForwardMeshForwardDrawLayout"}
                ).value();

            // Feed the _vp vertex shader the global VBO layout spec. Skinned
            // compute writes the same 22-float layout (bone tail zero-padded),
            // so this single spec works for both.
            auto* vlr = ctx.globalRegistry().find<VertexLayoutRegistry>();
            const VertexLayoutId vp_read_layout = kDefaultVertexLayoutId;

            // Family → fragment shader. Adding a shading model in an existing
            // family takes zero changes here; a new lighting-technique family
            // needs a new case below (+ the matching builtin frag ID up top).
            // The pipeline registration itself lives in the shared base method
            // registerFamilyPipelines (same source of truth as the deferred path).
            auto* shading_registry =
                ctx.globalRegistry().find<ShadingModelRegistry>();
            assert(shading_registry &&
                   "ForwardMeshFeature: ShadingModelRegistry missing from global registry");

            auto resolveFragmentShader =
                [&](EShadingModel sm) -> const ShaderObject*
            {
                return resolveFragmentForFamily(sm, fs_unlit, fs_pbr, fs_stylized, fs_graph);
            };

            registerFamilyPipelines(base, &fwd_vs, vlr, vp_read_layout,
                                    *shading_registry, resolveFragmentShader);
        }

        // ---- MDC compact compute pipeline ----
        initCompactPipeline(cfg_.forward_compact_shader, "ForwardMeshCompactLayout");
    }

    // =========================================================================
    //  Render graph passes
    // =========================================================================

    void ForwardMeshFeature::addPasses(RGBuilder &builder)
    {
        auto &ctx = renderContext();

        // ---- Cull + compact (shared GPU-driven view path; H9 de-dup) ----
        addCullAndCompactPasses(builder, CullCompactParams{
            .prefix                    = "Fwd",
            .phase                     = ECoreRenderPhase::ForwardOpaque,
            .domain                    = EPassDomain::ForwardOpaque,
            .cull_pass_name            = "ForwardMeshForwardCull",
            .compact_pass_name         = "ForwardMeshForwardCompact",
            .descriptor_layout_version = cfg_.descriptor_layout_version,
            .extension_flags           = cfg_.extension_flags,
        });

        auto visible_tds = builder.createTransientDS(
            "ForwardMeshVisibleDS",
            visible_set_layout_, {
                {0, EDescriptorType::STORAGE_BUFFER, visible_instance_rg_},
            });

        auto* mat_res = ctx.globalRegistry().find<MaterialResources>();
        auto variant_buckets = collectVariantBuckets(mat_res);
        // R1: give each graph material its own PSO (forward pass) before the
        // per-bucket pick() loop reads the override.
        registerGraphBucketPipelines(mat_res, /*use_gbuffer=*/false);

        // Per-scene bindless vertex-pool DS (set 7). Bound unconditionally so
        // skinned draws can read their output pool; static draws ignore set 7.
        auto* vpr = renderScene().sceneRegistry().find<VertexPoolRegistry>();
        const VkDescriptorSet vp_ds = vpr ? vpr->descriptorSet() : VK_NULL_HANDLE;

        // ---- Forward draw (graphics) ----
        auto &draw_pass = builder.addPass("ForwardMeshForwardDraw", ERGPassType::GRAPHICS)
            .write(builder.referenceTexture(cfg_.color_target), lux::common::ETextureRole::COLOR_ATTACHMENT)
            .write(builder.referenceTexture(cfg_.depth_target), lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
            .setPipeline(bucket_pipelines_.pick(0u, variant_buckets[0]))
            .bindSceneDS(0)
            .bindImmutableDS(1, instance_res_->descriptorSet())
            .bindImmutableDS(2, ctx.globalRegistry().descriptorSetOf<TextureResources>())
            .bindResourceDS(3, renderScene().sceneRegistry().find<LightResources>(), &LightResources::resolveDS, EDSBindMode::PER_FIF,
                            builder.trackExternalBuffer("ext.LightResources"), ERGResourceType::BUFFER)
            .bindResourceDS(4, ctx.globalRegistry().find<MaterialResources>(), &MaterialResources::resolveDS, EDSBindMode::PER_FIF,
                            builder.trackExternalBuffer("ext.MaterialResources"), ERGResourceType::BUFFER)
            .bindTransientDS(5, visible_tds)
            .read(draw_indirect_rg_, ERGBufferRole::INDIRECT)
            .read(draw_count_rg_, ERGBufferRole::INDIRECT)
            .read(visible_instance_rg_, ERGBufferRole::STORAGE)
            .after("ForwardMeshForwardCompact");

        {
            auto shadow_atlas = builder.referenceTexture(cfg_.shadow_atlas);
            draw_pass.read(shadow_atlas, lux::common::ETextureRole::SAMPLED);
            draw_pass.after("ShadowViewUpload");
        }

        // The variant array is one pipeline per VARIANT BUCKET (each resolved by
        // bucket_pipelines_.pick). Both static and skinned MDCs index the same
        // bucket; the kernel's `family_count` is left 0 (below) so it does not
        // apply the legacy `+ family_count` skinned offset. (Index 0 was set
        // above via setPipeline.)
        const uint32_t bucket_count = static_cast<uint32_t>(variant_buckets.size());
        for (uint32_t b = 1; b < bucket_count; ++b)
            draw_pass.addPipeline(bucket_pipelines_.pick(b, variant_buckets[b]));

        // Bind the bindless vertex pool at set 7 (shared 8-set layout).
        if (vp_ds != VK_NULL_HANDLE)
            draw_pass.bindImmutableDS(7, vp_ds);

        // Declare a read on every published compute-vertex producer's output
        // (skinning today; morph/cloth later) so the graph orders
        // producer(compute) → mesh draw and inserts the compute→vertex barrier.
        // The matching importBuffer lives in each producer's addPasses. Iterating
        // the registry means new producers are picked up here with ZERO changes
        // (replaces the hardcoded "SkinnedVertexPool" magic string). An
        // empty registry yields no reads, so non-producer scenes don't
        // dead-prune this draw pass against a missing resource.
        if (auto* vpr = renderScene().sceneRegistry().find<VertexProductionRegistry>())
        {
            for (const auto& prod : vpr->producers())
            {
                auto pool_rg = builder.referenceBuffer(prod.rg_buffer_name);
                draw_pass.read(pool_rg, ERGBufferRole::STORAGE);
            }
        }

        // One variant-feature entry per bucket (no skinned half).
        std::vector<uint32_t> variant_features;
        variant_features.reserve(bucket_count);
        for (const auto& bucket : variant_buckets)
            variant_features.push_back(bucket.feature_mask);
        draw_pass.setPipelineVariantFeatures(variant_features);

        draw_pass.setKernel("MeshDraw", makeKernelConfig(MeshDrawKernelConfig{
            .draw_count_rg  = draw_count_rg_,
            .indirect_rg    = draw_indirect_rg_,
            .geometry_mask  = supportedGeometryMask(EPassDomain::ForwardOpaque),
            .mdc_count      = mdcCount(),
            .mdc_entries    = instance_res_->mdcTable().entries().data(),
            // family_count = 0 ⇒ kernel skips the legacy `+ family_count`
            // skinned-variant offset; static + skinned share one pipeline.
            .family_count   = 0u,
        }));
    }
} // namespace lux::render
