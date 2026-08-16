#include <lux/engine/render/renderer/features/deferred/DeferredGBufferFeature.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/function/render/client/genops/DeferredGBufferOperation.ops.hpp>  // kDeferredGBufferKnownExtFlags
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>   // set-7 bind
#include <lux/engine/render/gpu/pipeline/VertexLayoutRegistry.hpp>  // vertex-layout SSOT
#include <lux/engine/render/gpu/pipeline/VertexLayoutSpec.hpp>      // appendVertexLayoutSpecs
#include <lux/engine/render/resources/vertex/VertexProduction.hpp>     // producer registry
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/function/render/client/features/deferred/DeferredGBufferOperation.hpp>   // kDeferredGBufferDrawPassName
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/VulkanCheck.hpp>
#include <lux/engine/render/resources/material/MaterialFamily.hpp>
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
        releaseAll();
    }

    void DeferredGBufferFeature::releaseAll() noexcept
    {
        // §2.2: Layout owned by DescriptorService — no manual destroy.
        visible_set_layout_ = VK_NULL_HANDLE;
        destroyCommon();
    }

    // =========================================================================
    //  Lifecycle
    // =========================================================================

    lux::render::Expected<void> DeferredGBufferFeature::initAndAttachTo(RenderScene & /*scene*/){
        return init();  // propagate init failure instead of swallowing it (audit feat-init)
    }

    void DeferredGBufferFeature::onDetachFromScene(RenderScene & /*scene*/)
    {
        releaseAll();
    }

    // =========================================================================
    //  Initialisation
    // =========================================================================

    Expected<void> DeferredGBufferFeature::init()
    {
        // 空句柄回填内置默认。任一解析不出模块即整体失败,而不是留一个无效句柄
        // 一路传到取模块时才炸。
        {
            auto& shaders = renderContext().globalRegistry().must<ShaderResources>();
            const std::array backfill{
                ShaderStageSlot{EBuiltinShader::MESH_CULL_UNIFIED_COMP, &cfg_.cull_compute_shader},
                ShaderStageSlot{EBuiltinShader::MDC_COMPACT_COMP,       &cfg_.compact_compute_shader},
                ShaderStageSlot{EBuiltinShader::GBUFFER_VERT,           &cfg_.gbuffer_vertex_shader},
                ShaderStageSlot{EBuiltinShader::GBUFFER_UNLIT_FRAG,     &cfg_.gbuffer_unlit_fragment_shader},
                ShaderStageSlot{EBuiltinShader::GBUFFER_PBR_FRAG,       &cfg_.gbuffer_pbr_fragment_shader},
                ShaderStageSlot{EBuiltinShader::GBUFFER_STYLIZED_FRAG,  &cfg_.gbuffer_stylized_fragment_shader}};
            if (auto filled = resolveShaderStages(shaders, backfill); !filled)
                return filled;
        }

        // ---- Common infrastructure (instance storage, buffers, cull) ----
        // Shadow rendering is handled separately by MeshShadowFeature.
        // Propagate a hard prerequisite failure so the feature isn't registered as
        // installed (never expected — Deferred ships with StandardMeshStack). (feat-init)
        if (auto r = initCommon(cfg_.cull_compute_shader, cfg_.extension_flags); !r)
            return r;

        auto &ctx = renderContext();
        auto& shaders = ctx.globalRegistry().must<ShaderResources>();
        assert(shaders.get(cfg_.gbuffer_vertex_shader) && "DeferredGBufferFeature: gbuffer_vertex_shader is invalid");
        assert(shaders.get(cfg_.gbuffer_unlit_fragment_shader) && "DeferredGBufferFeature: gbuffer_unlit_fragment_shader index is invalid");
        assert(shaders.get(cfg_.gbuffer_pbr_fragment_shader) && "DeferredGBufferFeature: gbuffer_pbr_fragment_shader index is invalid");
        assert(shaders.get(cfg_.gbuffer_stylized_fragment_shader) && "DeferredGBufferFeature: gbuffer_stylized_fragment_shader index is invalid");
        // Graph family frag is OPTIONAL (no builtin). null handle => Graph family
        // gets no pipeline (registerFamilyPipelines skips it).

        if (visible_set_layout_ == VK_NULL_HANDLE)
        {
            auto id = ctx.descriptorService().registerLayout(
                storageBufferVertexLayout("GBufferVisibleSetLayout"));
            visible_set_layout_ = ctx.descriptorService().layout(id);
        }

        // ---- GBuffer graphics pipelines (MRT: 3 color + depth, per family) ----
        // A single pipeline per (family, cull_mode) — the _vp shader
        // reads vertices from set 7 for BOTH static (via StaticVertexPoolSet) and
        // skinned (via SkinningResources transient pool) draws. Both share
        // the SAME stride (full 22-float layout 0), so one spec set serves
        // all draws. Mirrors ForwardMeshFeature.
        {
            auto base = makeOpaqueMeshTemplate();
            base.descriptor_set_count = 8;
            // The vertex/fragment modules are uniformly filled in by
            // registerFamilyPipelines internally, using the post-switch-over
            // handle (centralized there) — no need to pre-fill them here.
            base.blend_enable = VK_FALSE;
            base.depth_test_enable = VK_TRUE;
            base.depth_write_enable = VK_TRUE;
            base.depth_compare_op = VK_COMPARE_OP_LESS;

            // Reflected layout (same as ForwardMesh — engine_set members are
            // routed to the shared table; set 5 visible is built from
            // reflection and deduplicated with visible_set_layout_'s content
            // into the same object; set 6's placeholder hole is filled with
            // the Compute layout per its slot semantics). The explicit PC
            // declaration stays authoritative.
            base.debug_name = "DeferredGBufferDraw";
            base.push_constant_ranges.push_back({VK_SHADER_STAGE_VERTEX_BIT, 0, kViewPushPrefixSize});

            // Local-read merged-scope remaps: when the lighting
            // consumer merges into this scope, the union gains its lit-color
            // output at slot 3 — this producer writes fragment locations 0-2
            // and leaves slot 3 unused. Must bit-match the planner maps and
            // the LocalReadBoundary command-buffer state.
            // 旗标是客户端的选择,这里只检查它可不可行 —— 与 DeferredLightingFeature 的
            // INPUT_ATTACHMENT 检查同一条判据(两者必须落在同一个 caps 位上)。
            local_read_scope_ = cfg_.extension_flags.containsAll(EGpuDrivenMeshExt::LocalReadScope);
            if (local_read_scope_)
            {
                if (!ctx.deviceContext().caps().dynamic_rendering_local_read)
                    return renderFailure<err::lighting::LocalReadUnsupported>();
                base.lr_color_locations = {0u, 1u, 2u, VK_ATTACHMENT_UNUSED};
            }

            // Feed the _vp gbuffer shader the global VBO layout spec.
            // Skinned compute writes the same 22-float layout (bone tail
            // zero-padded) so one spec works for both.
            auto& vlr = ctx.globalRegistry().must<VertexLayoutRegistry>();
            const VertexLayoutId vp_read_layout = kDefaultVertexLayoutId;

            // Family → fragment shader (mirrors ForwardMeshFeature). Adding a
            // shading model in an existing family takes zero changes here; a new
            // lighting-technique family needs a new case below. The pipeline
            // registration itself lives in the shared base method
            // registerFamilyPipelines (same source of truth as the forward path).
            auto resolveFragmentShader =
                [this](EShadingModel sm) -> ShaderHandle
            {
                return resolveFragmentForFamily(sm, cfg_.gbuffer_unlit_fragment_shader,
                                                cfg_.gbuffer_pbr_fragment_shader,
                                                cfg_.gbuffer_stylized_fragment_shader,
                                                cfg_.gbuffer_graph_fragment_shader);
            };

            if (auto family = registerFamilyPipelines(
                    base,
                    cfg_.gbuffer_vertex_shader,
                    vlr,
                    vp_read_layout,
                    resolveFragmentShader
                ); !family)
                return family;
        }

        // ---- Compact compute pipeline (per-MDC, replaces finalize for view path) ----
        if (auto r = initCompactPipeline(cfg_.compact_compute_shader, "DeferredGBufferCompactLayout"); !r)
            return lux::cxx::unexpected(r.error());   // propagate, don't swallow
        return {};
    }

    // =========================================================================
    //  Render graph passes
    // =========================================================================

    void DeferredGBufferFeature::addPasses(RGBuilder &builder)
    {
        auto &ctx = renderContext();

        // ---- Create GBuffer transient textures ----
        // LOCAL_READ 作用域激活时消费者(lighting)以 input attachment 读,
        // G-buffer 不再需要 SAMPLED——声明纯 attachment usage 让分配器走
        // TRANSIENT_ATTACHMENT + LAZILY_ALLOCATED(tiler 上零物理显存)。
        // SAMPLED 读模式(无 local_read 能力的设备)保持原声明。
        const bool lr_scope = local_read_scope_;
        const ERGTextureUsageFlags gbuf_usage =
            static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::COLOR_ATTACHMENT)
            | static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::INPUT_ATTACHMENT)
            | (lr_scope ? static_cast<ERGTextureUsageFlags>(0)
                        : static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::SAMPLED));

        RGTextureDescription albedo_desc = RGTextureDescription::Relative(1.0f, 1.0f, lux::common::ETextureFormat::RGBA8_UNORM);
        albedo_desc.usage = gbuf_usage;
        auto gbuf_albedo = builder.createTexture(cfg_.gbuffer.albedo_metallic, albedo_desc);

        RGTextureDescription normal_desc = RGTextureDescription::Relative(1.0f, 1.0f, lux::common::ETextureFormat::RGBA16_SFLOAT);
        normal_desc.usage = gbuf_usage;
        auto gbuf_normal = builder.createTexture(cfg_.gbuffer.normal_roughness, normal_desc);

        auto emissive_fmt = renderScene().pipelineConfig().isLdr()
                                ? lux::common::ETextureFormat::RGBA8_UNORM
                                : lux::common::ETextureFormat::RGBA16_SFLOAT;
        RGTextureDescription emissive_desc = RGTextureDescription::Relative(1.0f, 1.0f, emissive_fmt);
        emissive_desc.usage = gbuf_usage;
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

        // ---- Pass 3: GBuffer draw (graphics, MRT) ----
        auto draw_pass = builder.addPass(kDeferredGBufferDrawPassName, ERGPassType::GRAPHICS)
                             .write(gbuf_albedo, lux::common::ETextureRole::COLOR_ATTACHMENT)
                             .write(gbuf_normal, lux::common::ETextureRole::COLOR_ATTACHMENT)
                             .write(gbuf_emissive, lux::common::ETextureRole::COLOR_ATTACHMENT)
                             .write(builder.referenceTexture(cfg_.depth_target), lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
                             .setPipeline(bucket_pipelines_.pick(0u, variant_buckets[0]))
                             .bindSceneDS()
                             .useEngineSet(EDescriptorSetSlot::Instance)
                             .bindImmutableDS(EDescriptorSetSlot::Texture, ctx.globalRegistry().descriptorSetOf<TextureResources>())
                             .useEngineSet(EDescriptorSetSlot::Material,
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
        if (vpr && vpr->isInitialized())
            draw_pass.useEngineSet(EDescriptorSetSlot::VertexPool);

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

        const auto index_buffers = importSharedIndexBuffers(builder);
        draw_pass.setKernel("MeshDraw", 
            makeKernelConfig(
                MeshDrawKernelConfig{
                    .draw_count_rg = draw_count_rg_,
                    .indirect_rg = draw_indirect_rg_,
                    .index_buffers_rg = index_buffers.data(),
                    .index_buffer_count = static_cast<std::uint32_t>(
                        index_buffers.size()),
                    .geometry_mask = supportedGeometryMask(),
                    .mdc_count = mdcCount(),
                    .mdc_entries = instance_res_->mdcTable().entries().data(),
                    // family_count = 0 skips skinned `+N` offset.
                    .family_count = 0u,
                }
            )
        );
    }
} // namespace lux::render
