#include <lux/engine/render/renderer/features/forward/ForwardMeshFeature.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/resources/lighting/LightResources.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>   // set-7 bind
#include <lux/engine/render/resources/vertex/VertexProduction.hpp>     // producer registry
#include <lux/engine/render/gpu/pipeline/VertexLayoutRegistry.hpp>  // vertex-layout SSOT
#include <lux/engine/render/gpu/pipeline/VertexLayoutSpec.hpp>      // appendVertexLayoutSpecs
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/function/render/client/features/shadow/ShadowMapOperation.hpp>             // kShadowViewUploadPassName
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
        return init();  // propagate init failure instead of swallowing it (audit feat-init)
    }

    // =========================================================================
    //  Initialisation
    // =========================================================================

    Expected<void> ForwardMeshFeature::init()
    {
        // ---- 内置着色器必须在进来之前就解析好 ----
        //
        // 唯一创建路径是 handler(ForwardMeshOperationHandlers.cpp:58 是全仓
        // 唯一的 addFeature<ForwardMeshFeature>),而 handler 侧有一张权威的
        // 回填表(MeshCommConfigValidation.hpp)负责按 CommConfig 解析。
        //
        // 此前这里另有一份逐行内置回填的副本。它是**防御性死代码**:对已解析的非空
        // 句柄回填是 no-op,所以今天不触发;
        // 但它缺少 handler 表的 HZB 条件分支(表在 HZB 旗标命中时选
        // MESH_CULL_UNIFIED_COMP_HZB,这份永远选非 HZB 变体)—— 是一个会与
        // 权威表悄悄分叉的**第二真相源**,而分叉后果正是那张表的注释警告的
        // "着色器与管线布局必须成对"。
        //
        // 改为断言:进来时必须已解析。真漏了就当场暴露,而不是拿一份可能过时的
        // 默认值兜住。
        assert(cfg_.forward_cull_shader.isValid() &&
               cfg_.forward_compact_shader.isValid() &&
               cfg_.forward_vert_shader.isValid() &&
               cfg_.unlit_fragment.isValid() &&
               cfg_.pbr_fragment.isValid() &&
               cfg_.stylized_fragment.isValid()
               && "ForwardMeshFeature: 着色器句柄应由 handler 侧的回填表解析完毕");

        // ---- Common infrastructure (instance storage, buffers, cull) ----
        // On a hard prerequisite failure (missing InstanceResources / cull shader —
        // never expected, Forward ships with StandardMeshStack) propagate the error so
        // the feature is NOT registered as successfully installed. (audit feat-init)
        if (auto r = initCommon(cfg_.forward_cull_shader, cfg_.extension_flags); !r)
            return r;

        auto &ctx = renderContext();
        auto& shaders = ctx.globalRegistry().must<ShaderResources>();
        assert(shaders.get(cfg_.forward_vert_shader) && "ForwardMeshFeature: forward_vert_shader is invalid");
        assert(shaders.get(cfg_.unlit_fragment) && "ForwardMeshFeature: unlit_fragment shader index is invalid");
        assert(shaders.get(cfg_.pbr_fragment) && "ForwardMeshFeature: pbr_fragment shader index is invalid");
        assert(shaders.get(cfg_.stylized_fragment) && "ForwardMeshFeature: stylized_fragment shader index is invalid");
        // Graph family frag is OPTIONAL (no builtin). null handle => Graph family
        // gets no forward pipeline (registerFamilyPipelines skips it).

        if (visible_set_layout_ == VK_NULL_HANDLE)
        {
            auto id = ctx.descriptorService().registerLayout(
                storageBufferVertexLayout("ForwardVisibleSetLayout"));
            visible_set_layout_ = ctx.descriptorService().layout(id);
        }

        // ---- Forward graphics pipelines (per material family bucket) ----
        // A single pipeline per (family, cull_mode) — the _vp shader
        // reads vertices from set 7 for BOTH static and skinned draws. The
        // global VBO segments are registered via StaticVertexPoolSet (per scene); skinned
        // outputs go through SkinningResources's transient pool. Both share
        // the SAME stride (full 22-float layout 0), so one spec set serves
        // all draws.
        {
            auto base = makeOpaqueMeshTemplate();
            base.descriptor_set_count = 8;
            // The vertex/fragment modules are uniformly filled in by
            // registerFamilyPipelines internally, using the post-switch-over
            // handle (centralized there) — no need to pre-fill them here.

            // Reflected layout — this replaces what used to be a hand-written
            // array of 8 sets. The engine_set members
            // (Scene/Instance/Texture/Light/Material/VertexPool) are routed
            // to the engine-shared table via the contract; set 5 visible is
            // a feature-private single binding, and its reflected shape
            // matches visible_set_layout_ (DescriptorService deduplicates
            // them into the same object by content); set 6 is a placeholder
            // hole that graphics doesn't use, filled with the shared Compute
            // layout per its slot semantics.
            //
            // PC is still declared explicitly: the 8-byte header at offset 0
            // is read by both VS (transform) and FS (material), and the
            // recorder pushes it using the template's stage set; if this
            // relied on reflection alone, some families' fragment shaders
            // with no PC block would drop the FRAGMENT bit and trigger
            // VUID-vkCmdPushConstants-01795.
            base.debug_name = "ForwardMeshDraw";
            base.push_constant_ranges.push_back(
                {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kViewPushPrefixSize});

            // Feed the _vp vertex shader the global VBO layout spec. Skinned
            // compute writes the same 22-float layout (bone tail zero-padded),
            // so this single spec works for both.
            auto& vlr = ctx.globalRegistry().must<VertexLayoutRegistry>();
            const VertexLayoutId vp_read_layout = kDefaultVertexLayoutId;

            // Family → fragment shader. Adding a shading model in an existing
            // family takes zero changes here; a new lighting-technique family
            // needs a new case below (+ the matching builtin frag ID up top).
            // The pipeline registration itself lives in the shared base method
            // registerFamilyPipelines (same source of truth as the deferred path).
            auto resolveFragmentShader =
                [this](EShadingModel sm) -> ShaderHandle
            {
                return resolveFragmentForFamily(sm, cfg_.unlit_fragment, cfg_.pbr_fragment,
                                                cfg_.stylized_fragment, cfg_.graph_fragment);
            };

            if (auto family = registerFamilyPipelines(
                    base,
                    cfg_.forward_vert_shader,
                    vlr,
                    vp_read_layout,
                    resolveFragmentShader
                ); !family)
                return family;
        }

        // ---- MDC compact compute pipeline ----
        if (auto r = initCompactPipeline(cfg_.forward_compact_shader, "ForwardMeshCompactLayout"); !r)
            return lux::cxx::unexpected(r.error());   // propagate, don't swallow
        return {};
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

        // ---- Forward draw (graphics) ----
        auto &draw_pass = builder.addPass("ForwardMeshForwardDraw", ERGPassType::GRAPHICS)
            .write(builder.referenceTexture(cfg_.color_target), lux::common::ETextureRole::COLOR_ATTACHMENT)
            .write(builder.referenceTexture(cfg_.depth_target), lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
            .setPipeline(bucket_pipelines_.pick(0u, variant_buckets[0]))
            .bindSceneDS()
            .useEngineSet(EDescriptorSetSlot::Instance)
            .bindImmutableDS(EDescriptorSetSlot::Texture, ctx.globalRegistry().descriptorSetOf<TextureResources>())
            .useEngineSet(EDescriptorSetSlot::Light,
                            builder.trackExternalBuffer("ext.LightResources"), ERGResourceType::BUFFER)
            .useEngineSet(EDescriptorSetSlot::Material,
                            builder.trackExternalBuffer("ext.MaterialResources"), ERGResourceType::BUFFER)
            .bindTransientDS(5, visible_tds)
            .read(draw_indirect_rg_, ERGBufferRole::INDIRECT)
            .read(draw_count_rg_, ERGBufferRole::INDIRECT)
            .read(visible_instance_rg_, ERGBufferRole::STORAGE)
            .after("ForwardMeshForwardCompact");

        {
            auto shadow_atlas = builder.referenceTexture(cfg_.shadow_atlas);
            draw_pass.read(shadow_atlas, lux::common::ETextureRole::SAMPLED);
            draw_pass.after(kShadowViewUploadPassName);
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
        if (vpr && vpr->isInitialized())
            draw_pass.useEngineSet(EDescriptorSetSlot::VertexPool);

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

        const auto index_buffers = importSharedIndexBuffers(builder);
        draw_pass.setKernel("MeshDraw", makeKernelConfig(MeshDrawKernelConfig{
            .draw_count_rg  = draw_count_rg_,
            .indirect_rg    = draw_indirect_rg_,
            .index_buffers_rg = index_buffers.data(),
            .index_buffer_count = static_cast<std::uint32_t>(
                index_buffers.size()),
            .geometry_mask  = supportedGeometryMask(),
            .mdc_count      = mdcCount(),
            .mdc_entries    = instance_res_->mdcTable().entries().data(),
            // family_count = 0 ⇒ kernel skips the legacy `+ family_count`
            // skinned-variant offset; static + skinned share one pipeline.
            .family_count   = 0u,
        }));
    }
} // namespace lux::render
