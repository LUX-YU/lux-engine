#include <lux/engine/render/renderer/features/deferred/DeferredLightingFeature.hpp>
#include <lux/engine/render/renderer/features/BufferTransferSynchronization.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/resources/lighting/LightResources.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/function/render/client/features/deferred/DeferredGBufferOperation.hpp>   // kDeferredGBufferDrawPassName
#include <lux/engine/function/render/client/features/shadow/MeshShadowOperation.hpp>            // kMeshShadowDrawPassName
#include <lux/engine/function/render/client/features/shadow/ShadowMapOperation.hpp>             // kShadowViewUploadPassName / kEvsmBlurVPassName
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraResource.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapFeature.hpp>       // attach 期 technique 交叉校验
#include <lux/engine/render/renderer/features/deferred/DeferredGBufferFeature.hpp> // attach 期 local-read 交叉校验
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>              // FeatureFactory 完整定义(genops 头只前置声明)
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>        // kShadowMapFeatureFactory.descriptor.type
#include <lux/engine/function/render/client/genops/DeferredGBufferOperation.ops.hpp>  // kDeferredGBufferFeatureFactory.descriptor.type
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/VulkanCheck.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>

#include <Eigen/Core>

namespace lux::render
{
    namespace
    {
#if !defined(LUX_SHADOW_DEBUG_ORDER)
#if !defined(NDEBUG)
#define LUX_SHADOW_DEBUG_ORDER 1
#else
#define LUX_SHADOW_DEBUG_ORDER 0
#endif
#endif

        [[nodiscard]] bool tryExtractPerspectiveNearFar(
            const Eigen::Matrix4f &proj, float &out_near, float &out_far)
        {
            constexpr float kEpsilon = 1e-5f;
            const float a = proj(2, 2);
            const float b = proj(2, 3);
            const float c = proj(3, 2);
            if (std::abs(a) < kEpsilon || std::abs(c) < kEpsilon || std::abs(a - c) < kEpsilon)
                return false;

            float near_v = std::abs(-b / a);
            float far_v = std::abs(-b / (a - c));
            if (!std::isfinite(near_v) || !std::isfinite(far_v))
                return false;
            if (far_v < near_v)
                std::swap(far_v, near_v);
            if (near_v <= 0.0f || far_v - near_v < kEpsilon)
                return false;

            out_near = near_v;
            out_far = far_v;
            return true;
        }
    }

    // =========================================================================
    //  Construction / destruction
    // =========================================================================

    DeferredLightingFeature::DeferredLightingFeature(Config cfg)
        : cfg_(std::move(cfg))
    {
    }

    DeferredLightingFeature::~DeferredLightingFeature()
    {
        destroy();
    }

    // =========================================================================
    //  Lifecycle
    // =========================================================================

    lux::render::Expected<void> DeferredLightingFeature::initAndAttachTo(RenderScene &scene)
    {
        if (auto ready = init(); !ready)
            return ready;

        // attach 期的跨 feature 一致性校验。两条都靠 orchestrator 的装配顺序
        // (shadow / gbuffer 先于 lighting)才能看到对方 —— 装在前面的还没上来时
        // 扫不到,这一段就是空转,由那一侧自己的 caps 检查兜底。
        //
        // 认人按**稳定 type id**,不按 name()。本代码库不开 dynamic_cast,下面紧跟着
        // 的是无检查的 static_cast —— 认错人就是 UB,所以判据必须是不可能被第三方
        // 占用的东西。
        //
        // 此前用的是 `f->name() == XxxFeature::kFeatureName`。取常量而不敲字面量,
        // 挡住的只是**拼写与改名**;挡不住的是 `FeatureFactory.name` 按文档就是
        // **插件自选的字符串** —— 一个第三方特性把自己命名成 "ShadowMap",就会通过
        // 这个判断然后被 static_cast 成 ShadowMapFeature。而插件系统正在建。
        //
        // typeId() 是 SceneFeature 早就有的稳定身份(RenderScene 在安装期从工厂
        // 描述符填入),而描述符的 type 来自 LUX_COMM_CONFIG 的
        // `id=lux.render.shadow_map.v1` 反射标注。取工厂的 descriptor.type 而不是
        // 在这里写 featureId("lux.render.shadow_map.v1") 字面量:与注册**同源**,
        // 对方哪天把 .v1 提成 .v2,这里跟着变,不会静默失配。
        for (const auto *f : scene.features())
        {
            if (f == nullptr)
                continue;

            if (f->typeId() == kShadowMapFeatureFactory.descriptor.type)
            {
                // lighting 的 SPIR-V 变体按 cfg_.technique 选定(PCF 采 D32 图集 vs
                // EVSM 采 RGBA16F),与 ShadowMap 的活动 technique 错配就是采错图集。
                const auto *shadow_map = static_cast<const ShadowMapFeature *>(f);
                if (shadow_map->activeTechnique() != cfg_.technique)
                    return renderFailure<err::lighting::ShadowTechniqueMismatch>(
                        static_cast<std::uint32_t>(cfg_.technique),
                        static_cast<std::uint32_t>(shadow_map->activeTechnique()));
            }
            else if (f->typeId() == kDeferredGBufferFeatureFactory.descriptor.type)
            {
                // G-buffer 写不写进合并作用域,与本 feature 读不读 input attachment,
                // 必须是同一个决定。上层的同一个 if 会一起写这两项;这里防的是
                // 只改了一边的配置。
                const auto *gbuffer = static_cast<const DeferredGBufferFeature *>(f);
                if (gbuffer->localReadScope() != effective_local_read_)
                    return renderFailure<err::lighting::ReadModeScopeMismatch>(
                        static_cast<std::uint32_t>(gbuffer->localReadScope()),
                        static_cast<std::uint32_t>(effective_local_read_));
            }
        }
        return {};
    }

    void DeferredLightingFeature::onDetachFromScene(RenderScene & /*scene*/)
    {
        // gbuffer_sampler_ 来自 DescriptorService 采样器缓存——服务持有
        // 生命周期,removeFeature UAF(#17 sibling)不复存在 — no manual
        // retire here. §2.2: layouts owned by DescriptorService.
        gbuffer_ds_layout_ = VK_NULL_HANDLE;
        cluster_ds_layout_ = VK_NULL_HANDLE;
        cluster_clear_ds_layout_ = VK_NULL_HANDLE;
    }

    void DeferredLightingFeature::onFrameBegin(const FeatureFrameContext & /*ctx*/)
    {
        // clustered_enabled is decided at graph-compile time (addPasses) from the
        // live light count, but adding/removing lights does NOT invalidate the
        // render graph — only feature/instance/MDC changes do. So a scene that
        // starts below the threshold and later gains many lights kept executing the
        // brute-force per-pixel light loop (the exact case clustering exists for),
        // and one that drops below kept paying for the cluster compute passes, until
        // some unrelated change forced a recompile. Watch the count here and request
        // a recompile when it crosses the threshold in either direction so the
        // cluster passes are added/dropped to match. Rendering stays correct either
        // way (the shader's grid.w==0 fallback), so this is purely a perf fix. (perf)
        if (light_cache_ == nullptr)
            light_cache_ = renderScene().sceneRegistry().find<LightResources>();
        auto *light_res = light_cache_;
        const uint32_t light_count =
            light_res ? (light_res->lightCount<PointLightGPU>()
                       + light_res->lightCount<SpotLightGPU>())
                      : 0u;
        if (clusteringWanted(light_count) != clustering_compiled_)
            renderScene().invalidateGraph(
                EGraphInvalidationReason::FEATURE_TOPOLOGY);
    }

    // =========================================================================
    //  Initialisation
    // =========================================================================

    Expected<void> DeferredLightingFeature::init()
    {
        auto &ctx = renderContext();
        VkDevice device = ctx.deviceContext().logicalDevice();
        auto& shaders = ctx.globalRegistry().must<ShaderResources>();

        // 读模式是客户端的选择,这里只检查它可不可行。此前这一行写的是
        // `read_mode != SAMPLED && caps.dynamic_rendering_local_read` —— 设备不支持就
        // 悄悄退回 SAMPLED,客户端拿到一个装配成功的回执,却不知道自己要的 tile-local
        // 快路没生效。两条路径视觉等价但显存流量差一个量级,该不该退是上层的决定。
        effective_local_read_ = (cfg_.read_mode == EReadMode::INPUT_ATTACHMENT);
        if (effective_local_read_ && !ctx.deviceContext().caps().dynamic_rendering_local_read)
            return renderFailure<err::lighting::LocalReadUnsupported>();

        // 按 shadow technique × 读模式挑 SPIR-V 变体。调用方显式覆盖了 fragment_shader
        // 的话,回填会原样保留它。
        const bool evsm = cfg_.technique == EShadowTechnique::EVSM;
        const auto frag_variant = effective_local_read_
            ? (evsm ? EBuiltinShader::DEFERRED_LIGHTING_FRAG_EVSM_LR
                    : EBuiltinShader::DEFERRED_LIGHTING_FRAG_PCF_LR)
            : (evsm ? EBuiltinShader::DEFERRED_LIGHTING_FRAG_EVSM
                    : EBuiltinShader::DEFERRED_LIGHTING_FRAG_PCF);

        const std::array backfill{
            ShaderStageSlot{EBuiltinShader::DEFERRED_LIGHTING_VERT,   &cfg_.vertex_shader},
            ShaderStageSlot{frag_variant,                             &cfg_.fragment_shader},
            ShaderStageSlot{EBuiltinShader::CLUSTER_BUILD_COMP,       &cfg_.cluster_build_shader},
            ShaderStageSlot{EBuiltinShader::CLUSTER_COUNT_COMP,       &cfg_.cluster_count_shader},
            ShaderStageSlot{EBuiltinShader::CLUSTER_SCAN_COMP,        &cfg_.cluster_scan_shader},
            ShaderStageSlot{EBuiltinShader::CLUSTER_FILL_COMP,        &cfg_.cluster_fill_shader},
            ShaderStageSlot{EBuiltinShader::CLEAR_COUNT_BUFFERS_COMP, &cfg_.cluster_clear_shader}};
        if (auto filled = resolveShaderStages(shaders, backfill); !filled)
            return filled;

        // 域合并切换 —— 这是第一条含多个 FEATURE 域成员的管线。
        //
        // Light set(含 b4-b10 的阴影段,由契约的完整性对账补齐)从规范 set 3 搬进
        // FEATURE 域槽 2 的 [+2, +13) 区间;uViews 留在恒等位;GBuffer 的私有 set 1 与
        // cluster 的显式 set 3 不动。录制期 Light 的逻辑 binding collapse 成一次域集绑定
        //(数据一致性由 LightResources/ShadowResources 的双写保证)。切换之后这条管线
        // 正好落在 4-set 的目标形状上。
        const std::array lighting_stages{cfg_.vertex_shader, cfg_.fragment_shader};
        auto stages = shaders.preparePipelineStages(lighting_stages);
        if (!stages)
            return lux::cxx::unexpected(stages.error());

        // (The GBuffer set-1 layout is built from reflection — see below,
        //  fetched via templateSetLayout after the lighting pipeline is
        //  registered; it's only used by this pipeline, so its shape is
        //  exactly what reflection sees.)

        // ---- Clustered deferred descriptor set layout ----
        // A SET OWNED AND SHARED BY THE FEATURE ACROSS PIPELINES: the
        // lighting graphics pipeline and all 4 cluster compute pipelines
        // bind the same cluster DS, but each only uses a subset of its
        // bindings, so the shape must be explicitly declared by this feature
        // — it cannot be inferred from any single pipeline's reflection
        // (which is exactly what the explicit_set_layouts channel is for).
        {
            const std::array<VkDescriptorSetLayoutBinding, 6> bindings{{
                {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            }};

            auto id = ctx.descriptorService().registerLayout({.bindings = bindings,
                                                              .debug_name = "ClusterDSLayout"});
            cluster_ds_layout_ = ctx.descriptorService().layout(id);
        }

        // (Counter-clear set: clear_count_buffers.comp uses all 8 bindings,
        //  so its shape is exactly what reflection sees -> built purely from
        //  reflection, fetched via computeSetLayout after registration.)

        // ---- G-buffer 采样器:共享缓存(最近邻 clamp,不得插值)----
        gbuffer_sampler_ = ctx.descriptorService().sampler(SamplerDesc::nearestClamp());

        // ---- Fullscreen lighting pipeline ----
        //
        // Non-standard DS layout: Set 0 = Scene, Set 1 = GBuffer, Set 2 = Light+Shadow
        // We build the pipeline layout manually and set resource_slot_map so the
        // framework knows how to bind the Scene and Light DS at the correct slots.
        {
            GraphicsPipelineTemplate tmpl{};
            tmpl.geometry_type = EGeometryType::MESH;
            tmpl.vertex_shader = stages->module(0);
            tmpl.fragment_shader = stages->module(1);
            tmpl.descriptor_set_count = 4;

            // No vertex input (fullscreen triangle generated in vertex shader)
            tmpl.vertex_bindings.clear();
            tmpl.vertex_attributes.clear();

            // No depth
            tmpl.depth_test_enable = VK_FALSE;
            tmpl.depth_write_enable = VK_FALSE;

            // No blending (write HDR color directly)
            tmpl.blend_enable = VK_FALSE;

            // Back-face cull off (fullscreen triangle)
            tmpl.cull_mode = VK_CULL_MODE_NONE;

            // Non-standard resource_slot_map:
            //   Scene → Vulkan set 0
            //   Light → Vulkan set 2
            // (GBuffer at set 1 has no standard EDescriptorSetSlot mapping;
            //  it is bound manually via bindImmutableDS().)
            tmpl.resource_slot_map.push_back({EDescriptorSetSlot::Scene, 0});
            tmpl.resource_slot_map.push_back({EDescriptorSetSlot::Light, 2});

            // Reflected layout: set 0 Scene / set 2 Light are engine_set
            // (routed to the shared table via the contract); set 1 GBuffer
            // is built from reflection (private to this pipeline); set 3
            // cluster is a set owned and shared by the feature, declared
            // explicitly.
            tmpl.debug_name = "DeferredLighting";
            tmpl.explicit_set_layouts.push_back({3u, cluster_ds_layout_});

            // Local-read merged-scope remaps. Scope shape is
            // deterministic from this feature's own contract: 3 G-buffer
            // colors (slots 0-2, read as input attachments 0-2) + this pass's
            // lit-color output appended at slot 3 (fragment location 0);
            // scope depth read as input attachment 3. MUST bit-match the
            // planner's per-pass maps (both derive from the same shape).
            if (effective_local_read_)
            {
                tmpl.lr_color_locations = {VK_ATTACHMENT_UNUSED, VK_ATTACHMENT_UNUSED,
                                           VK_ATTACHMENT_UNUSED, 0u};
                tmpl.lr_input_indices   = {0u, 1u, 2u, VK_ATTACHMENT_UNUSED};
                tmpl.lr_depth_input_index = 3u;
            }
            // Authoritative PC ranges — prevents reflection from dropping
            // VERTEX_BIT (fullscreen vertex shader has no PC block).
            tmpl.push_constant_ranges.push_back(
                {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kViewPushPrefixSize});

            auto pipeline = ctx.pipelineManager().registerGraphicsTemplate(tmpl, stages->infos());
            if (!pipeline)
                return lux::cxx::unexpected(pipeline.error());

            lighting_pipeline_  = *pipeline;
            gbuffer_ds_layout_  = ctx.pipelineManager().templateSetLayout(lighting_pipeline_, 1);
            if (gbuffer_ds_layout_ == VK_NULL_HANDLE)
                return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(1);
        }

        auto create_cluster_pipeline = [&](ShaderHandle shader_handle, const char *debug_name) -> ComputePipelineHandle
        {
            if (shader_handle.isNull())
                return kInvalidComputePipelineHandle;

            // (原先这里有一句 `auto* shader_res = shaders;` 别名 + 两处
            //  `shader_res ? ... : fallback` —— 外层 shaders 已由 [&] 捕获,
            //  别名纯属噪音,而判空针对的是必然存在的全局单例。)
            // Compute-pipeline switch-over: Light moves from canonical set 1
            // (shader-local) into the [+2, +13) range of FEATURE domain slot
            // 2; cluster's explicitly-declared set 0 doesn't move. Layout
            // building shares buildReflectedPipelineLayout with the graphics
            // path, so domain mode takes effect naturally; at record time,
            // Light's logical bindings collapse into a domain-set bind via
            // the compute path.
            const std::array cluster_stages{shader_handle};
            auto prepared = shaders.preparePipelineStages(cluster_stages);
            if (!prepared)
                return kInvalidComputePipelineHandle;

            // set 0 = cluster (a set owned and shared by the feature,
            // declared explicitly); set 1 = Light (engine_set; after
            // switching over it lives in FEATURE domain slot 2).
            const std::array<std::pair<uint32_t, VkDescriptorSetLayout>, 1> explicit_sets{
                {{0u, cluster_ds_layout_}}};
            auto h = ctx.pipelineManager().registerComputePipelineReflected(
                prepared->module(0), prepared->info(0), debug_name, {}, explicit_sets);
            return h ? *h : kInvalidComputePipelineHandle;
        };

        auto create_clear_pipeline = [&](ShaderHandle shader_handle, const char *debug_name) -> ComputePipelineHandle
        {
            if (shader_handle.isNull())
                return kInvalidComputePipelineHandle;

            auto *shader_obj = shaders.get(shader_handle);
            if (!shader_obj)
                return kInvalidComputePipelineHandle;

            // clear's set 0 is used only by this pipeline, and the shader
            // uses all 8 bindings, so its shape is exactly what reflection
            // sees -> built purely from reflection; PC is also derived from
            // reflection.
            auto h = ctx.pipelineManager().registerComputePipelineReflected(
                shader_obj->module, shader_obj->info, debug_name);
            if (!h)
                return kInvalidComputePipelineHandle;
            cluster_clear_ds_layout_ = ctx.pipelineManager().computeSetLayout(*h, 0);
            return *h;
        };

        cluster_build_pipeline_ = create_cluster_pipeline(cfg_.cluster_build_shader, "ClusterBuildLayout");
        cluster_count_pipeline_ = create_cluster_pipeline(cfg_.cluster_count_shader, "ClusterCountLayout");
        cluster_scan_pipeline_ = create_cluster_pipeline(cfg_.cluster_scan_shader, "ClusterScanLayout");
        cluster_fill_pipeline_ = create_cluster_pipeline(cfg_.cluster_fill_shader, "ClusterFillLayout");
        cluster_clear_pipeline_ = create_clear_pipeline(cfg_.cluster_clear_shader, "ClusterClearCountersLayout");
        return {};
    }

    void DeferredLightingFeature::destroy() noexcept
    {
        // gbuffer_sampler_ 是缓存句柄,不归本特性销毁。
        // §2.2: Layouts owned by DescriptorService — no manual destroy.
        gbuffer_ds_layout_ = VK_NULL_HANDLE;
        cluster_ds_layout_ = VK_NULL_HANDLE;
        cluster_clear_ds_layout_ = VK_NULL_HANDLE;
    }

    // =========================================================================
    //  Render graph passes
    // =========================================================================

    void DeferredLightingFeature::addPasses(RGBuilder &builder)
    {
        auto &ctx = renderContext();

        // ---- Reference GBuffer resources (forward references resolved at compile time) ----
        auto gbuf_albedo = builder.referenceTexture(cfg_.gbuffer.albedo_metallic);
        auto gbuf_normal = builder.referenceTexture(cfg_.gbuffer.normal_roughness);
        auto gbuf_emissive = builder.referenceTexture(cfg_.gbuffer.emissive_ao);
        auto gbuf_depth = builder.referenceTexture(cfg_.depth_target);

        // ---- Lit color target ----
        // If the target resource already exists (e.g. imported backbuffer "SceneColor"
        // in LDR mode), reuse it to avoid shadowing the import with a new transient.
        auto existing = builder.findResource(cfg_.color_output);
        RGResourceHandle lit_color;
        if (existing)
        {
            lit_color = existing;
        }
        else
        {
            RGTextureDescription lit_desc = RGTextureDescription::Relative(
                1.0f, 1.0f, renderScene().pipelineConfig().lit_color_format);
            lit_desc.usage = static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::COLOR_ATTACHMENT) | static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::SAMPLED);
            lit_color = builder.createTexture(cfg_.color_output, lit_desc);
        }

        // ---- Transient GBuffer descriptor set (per-view per-frame) ----
        // SAMPLED path: combined samplers over the finished G-buffer.
        // LOCAL_READ path: input-attachment descriptors over the
        // same bindings — the whole scope holds them in RENDERING_LOCAL_READ.
        auto gbuffer_tds = effective_local_read_
            ? builder.createTransientDS("GBufferDS", gbuffer_ds_layout_, {
                  {0, EDescriptorType::INPUT_ATTACHMENT, gbuf_albedo,   {}, EImageLayout::RENDERING_LOCAL_READ},
                  {1, EDescriptorType::INPUT_ATTACHMENT, gbuf_normal,   {}, EImageLayout::RENDERING_LOCAL_READ},
                  {2, EDescriptorType::INPUT_ATTACHMENT, gbuf_emissive, {}, EImageLayout::RENDERING_LOCAL_READ},
                  {3, EDescriptorType::INPUT_ATTACHMENT, gbuf_depth,    {}, EImageLayout::RENDERING_LOCAL_READ},
              })
            : builder.createTransientDS("GBufferDS", gbuffer_ds_layout_, {
                  {0, EDescriptorType::COMBINED_IMAGE_SAMPLER, gbuf_albedo, gbuffer_sampler_, EImageLayout::SHADER_READ_ONLY_OPTIMAL},
                  {1, EDescriptorType::COMBINED_IMAGE_SAMPLER, gbuf_normal, gbuffer_sampler_, EImageLayout::SHADER_READ_ONLY_OPTIMAL},
                  {2, EDescriptorType::COMBINED_IMAGE_SAMPLER, gbuf_emissive, gbuffer_sampler_, EImageLayout::SHADER_READ_ONLY_OPTIMAL},
                  {3, EDescriptorType::COMBINED_IMAGE_SAMPLER, gbuf_depth, gbuffer_sampler_, EImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL},
              });

        // ---- Reference shadow atlas (forward ref — if ShadowMapFeature absent, pass is pruned) ----
        auto shadow_atlas = builder.referenceTexture(cfg_.shadow_atlas);

        if (light_cache_ == nullptr)
            light_cache_ = renderScene().sceneRegistry().find<LightResources>();
        auto *light_res = light_cache_;

        const uint32_t cluster_x = std::max(cfg_.cluster_x, 1u);
        const uint32_t cluster_y = std::max(cfg_.cluster_y, 1u);
        const uint32_t cluster_z = std::max(cfg_.cluster_z, 1u);
        const uint32_t cluster_count = cluster_x * cluster_y * cluster_z;

        // ClusterParamsGPU 已提到 DeferredLightingFeature.hpp(带 sizeof 断言)——
        // 它是 GLSL 侧 5 份手抄的 ABI 镜像,住在函数体里既无法被断言看住、
        // 也无法被任何头文件引用。
        RGBufferDescription params_desc{};
        params_desc.size = sizeof(ClusterParamsGPU);
        params_desc.stride = sizeof(ClusterParamsGPU);
        params_desc.element_count = 1;
        params_desc.usage = ERGBufferUsageBits::UNIFORM | ERGBufferUsageBits::TRANSFER_DST;
        params_desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
        auto cluster_params = builder.createBuffer("ClusterParams", params_desc);

        RGBufferDescription counts_desc{};
        counts_desc.size = static_cast<VkDeviceSize>(cluster_count) * sizeof(uint32_t);
        counts_desc.stride = sizeof(uint32_t);
        counts_desc.element_count = cluster_count;
        counts_desc.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::TRANSFER_DST;
        counts_desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
        auto cluster_counts = builder.createBuffer("ClusterCounts", counts_desc);

        RGBufferDescription offsets_desc{};
        offsets_desc.size = static_cast<VkDeviceSize>(cluster_count + 1u) * sizeof(uint32_t);
        offsets_desc.stride = sizeof(uint32_t);
        offsets_desc.element_count = cluster_count + 1u;
        offsets_desc.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::TRANSFER_DST;
        offsets_desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
        auto cluster_offsets = builder.createBuffer("ClusterOffsets", offsets_desc);

        RGBufferDescription write_head_desc{};
        write_head_desc.size = static_cast<VkDeviceSize>(cluster_count) * sizeof(uint32_t);
        write_head_desc.stride = sizeof(uint32_t);
        write_head_desc.element_count = cluster_count;
        write_head_desc.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::TRANSFER_DST;
        write_head_desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
        auto cluster_write_heads = builder.createBuffer("ClusterWriteHeads", write_head_desc);

        RGBufferDescription indices_desc{};
        indices_desc.size = static_cast<VkDeviceSize>(std::max(cfg_.max_cluster_indices, 1u)) * sizeof(uint32_t);
        indices_desc.stride = sizeof(uint32_t);
        indices_desc.element_count = std::max(cfg_.max_cluster_indices, 1u);
        indices_desc.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::TRANSFER_DST;
        indices_desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
        auto cluster_indices = builder.createBuffer("ClusterIndices", indices_desc);

        RGBufferDescription overflow_desc{};
        overflow_desc.size = sizeof(uint32_t);
        overflow_desc.stride = sizeof(uint32_t);
        overflow_desc.element_count = 1;
        overflow_desc.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::TRANSFER_DST;
        overflow_desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
        auto cluster_overflow = builder.createBuffer("ClusterOverflow", overflow_desc);

        auto cluster_tds = builder.createTransientDS(
            "ClusterDS", 
            cluster_ds_layout_, 
            {
                {0, EDescriptorType::UNIFORM_BUFFER, cluster_params},
                {1, EDescriptorType::STORAGE_BUFFER, cluster_counts},
                {2, EDescriptorType::STORAGE_BUFFER, cluster_offsets},
                {3, EDescriptorType::STORAGE_BUFFER, cluster_write_heads},
                {4, EDescriptorType::STORAGE_BUFFER, cluster_indices},
                {5, EDescriptorType::STORAGE_BUFFER, cluster_overflow},
            }
        );

        // Clustered light culling only pays off once the light count is high
        // enough to amortise the 5 cluster compute passes (clear/build/count/
        // scan/fill over a 16×9×24 = 3456-cell grid). Below the threshold the
        // brute-force per-pixel light loop (the existing enable_clustered=0
        // fallback in deferred_lighting.frag) is cheaper. ClusterBuild always
        // runs and writes grid.w = clustered_enabled ? 1 : 0, so the shader
        // takes the right path; the Count/Scan/Fill passes are skipped.
        const uint32_t lighting_light_count =
            light_res ? (light_res->lightCount<PointLightGPU>()
                       + light_res->lightCount<SpotLightGPU>())
                      : 0u;
        const bool clustered_enabled = clusteringWanted(lighting_light_count);
        // Record the decision the graph is being compiled with so onFrameBegin can
        // detect a later threshold crossing and request a recompile. (perf)
        clustering_compiled_ = clustered_enabled;
        const bool clear_counters_enabled =
            clustered_enabled && cluster_clear_pipeline_.valid();

        if (clear_counters_enabled)
        {
            auto cluster_clear_tds = builder.createTransientDS("ClusterClearDS", cluster_clear_ds_layout_, {
                                                                                                               {0, EDescriptorType::STORAGE_BUFFER, cluster_counts},
                                                                                                               {1, EDescriptorType::STORAGE_BUFFER, cluster_offsets},
                                                                                                               {2, EDescriptorType::STORAGE_BUFFER, cluster_write_heads},
                                                                                                               {3, EDescriptorType::STORAGE_BUFFER, cluster_overflow},
                                                                                                               {4, EDescriptorType::STORAGE_BUFFER, cluster_overflow},
                                                                                                               {5, EDescriptorType::STORAGE_BUFFER, cluster_overflow},
                                                                                                               {6, EDescriptorType::STORAGE_BUFFER, cluster_overflow},
                                                                                                               {7, EDescriptorType::STORAGE_BUFFER, cluster_overflow},
                                                                                                           });

            ClearCountersKernelConfig clear_cfg{};
            clear_cfg.buffers[0] = cluster_counts;
            clear_cfg.buffers[1] = cluster_offsets;
            clear_cfg.buffers[2] = cluster_write_heads;
            clear_cfg.buffers[3] = cluster_overflow;
            clear_cfg.buffer_count = 4u;

            builder.addPass("ClusterClearCounters", ERGPassType::COMPUTE)
                .setComputePipeline(cluster_clear_pipeline_)
                .bindTransientDS(0, cluster_clear_tds)
                .write(cluster_counts, ERGBufferRole::STORAGE)
                .write(cluster_offsets, ERGBufferRole::STORAGE)
                .write(cluster_write_heads, ERGBufferRole::STORAGE)
                .write(cluster_overflow, ERGBufferRole::STORAGE)
                .setKernel("ClearCounters", makeKernelConfig(clear_cfg));
        }

        auto cluster_build_pass = builder.addPass(
                                             "ClusterBuild",
                                             clustered_enabled ? ERGPassType::COMPUTE : ERGPassType::TRANSFER)
                                      .write(cluster_params, ERGBufferRole::CONSTANT)
                                      .write(cluster_counts, ERGBufferRole::STORAGE)
                                      .write(cluster_offsets, ERGBufferRole::STORAGE)
                                      .write(cluster_write_heads, ERGBufferRole::STORAGE)
                                      .write(cluster_indices, ERGBufferRole::STORAGE)
                                      .write(cluster_overflow, ERGBufferRole::STORAGE)
                                      .setKernelFn([this, light_res, clustered_enabled, clear_counters_enabled, cluster_x, cluster_y, cluster_z, cluster_count,
                                                    cluster_params, cluster_counts, cluster_offsets, cluster_write_heads, cluster_indices, cluster_overflow](const PassRecordContext &rec)
                                                   {
                VkBuffer params_buf = rec.resolveBufferHandle(cluster_params);
                VkBuffer counts_buf = rec.resolveBufferHandle(cluster_counts);
                VkBuffer offsets_buf = rec.resolveBufferHandle(cluster_offsets);
                VkBuffer heads_buf = rec.resolveBufferHandle(cluster_write_heads);
                VkBuffer indices_buf = rec.resolveBufferHandle(cluster_indices);
                VkBuffer overflow_buf = rec.resolveBufferHandle(cluster_overflow);
                if (params_buf == VK_NULL_HANDLE
                    || counts_buf == VK_NULL_HANDLE
                    || offsets_buf == VK_NULL_HANDLE
                    || heads_buf == VK_NULL_HANDLE
                    || indices_buf == VK_NULL_HANDLE
                    || overflow_buf == VK_NULL_HANDLE)
                {
                    return;
                }

                const uint32_t point_count = light_res ? light_res->lightCount<PointLightGPU>() : 0u;
                const uint32_t spot_count = light_res ? light_res->lightCount<SpotLightGPU>() : 0u;

                ClusterParamsGPU params{};
                if (rec.view)
                {
                    auto* cam = resolveViewCameraOnce(cam_cache_, renderScene().sceneRegistry());
                    const ViewFrameData* cam_fd = cam ? cam->find(rec.view->handle.index) : nullptr;
                    ViewFrameData vfd = cam_fd ? *cam_fd : ViewFrameData{};
                    const auto& cv = vfd.camera_view;
                    std::memcpy(params.view, cv.view.data(), sizeof(params.view));
                    params.view[12] = 0.0f;
                    params.view[13] = 0.0f;
                    params.view[14] = 0.0f;
                    std::memcpy(params.proj, cv.proj.data(), sizeof(params.proj));
                    for (std::size_t axis = 0; axis != 3u; ++axis)
                    {
                        params.camera_page[axis] = vfd.render_origin.page_delta[axis];
                        params.camera_local_page_size[axis] = vfd.render_origin.local[axis];
                    }
                    params.camera_local_page_size[3] = vfd.coordinate_page_size;
                    float near_z = 0.1f;
                    float far_z = 100.0f;
                    (void)tryExtractPerspectiveNearFar(cv.proj, near_z, far_z);
                    params.viewport[0] = rec.viewport.width;
                    params.viewport[1] = rec.viewport.height;
                    params.viewport[2] = near_z;
                    params.viewport[3] = far_z;
                }
                else
                {
                    params.viewport[0] = rec.viewport.width;
                    params.viewport[1] = rec.viewport.height;
                    params.viewport[2] = 0.1f;
                    params.viewport[3] = 100.0f;
                }
                params.grid[0] = cluster_x;
                params.grid[1] = cluster_y;
                params.grid[2] = cluster_z;
                params.grid[3] = clustered_enabled ? 1u : 0u;
                params.limits[0] = cluster_count;
                params.limits[1] = std::max(cfg_.max_cluster_indices, 1u);
                params.limits[2] = point_count;
                params.limits[3] = spot_count;

                synchronizeBeforeBufferTransferWrites(
                    rec.cmd,
                    std::array{params_buf}
                );
                vkCmdUpdateBuffer(rec.cmd, params_buf, 0, sizeof(ClusterParamsGPU), &params);

                if (!clear_counters_enabled)
                {
                    synchronizeBeforeBufferTransferWrites(
                        rec.cmd,
                        std::array{
                            counts_buf,
                            offsets_buf,
                            heads_buf,
                            overflow_buf}
                    );
                    vkCmdFillBuffer(rec.cmd, counts_buf, 0, VK_WHOLE_SIZE, 0u);
                    vkCmdFillBuffer(rec.cmd, offsets_buf, 0, VK_WHOLE_SIZE, 0u);
                    vkCmdFillBuffer(rec.cmd, heads_buf, 0, VK_WHOLE_SIZE, 0u);
                    vkCmdFillBuffer(rec.cmd, overflow_buf, 0, VK_WHOLE_SIZE, 0u);
                }

                VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                if (clear_counters_enabled)
                {
                    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    barrier.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
                }
                else
                {
                    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                    barrier.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                }
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(rec.cmd, &dep);

                if (clustered_enabled)
                {
                    vkCmdDispatch(rec.cmd, (cluster_count + 63u) / 64u, 1u, 1u);
                } })
                                      .setKernel("ClusterBuild");

        if (clustered_enabled)
            cluster_build_pass
                .setComputePipeline(cluster_build_pipeline_)
                .bindTransientDS(0, cluster_tds);
        if (clear_counters_enabled)
            cluster_build_pass.after("ClusterClearCounters");

        if (clustered_enabled)
        {
            builder.addPass("ClusterCount", ERGPassType::COMPUTE)
                .setComputePipeline(cluster_count_pipeline_)
                .bindTransientDS(0, cluster_tds)
                .useEngineSet(EDescriptorSetSlot::Light,
                                builder.trackExternalBuffer("ext.LightResources"), ERGResourceType::BUFFER)
                .read(cluster_params, ERGBufferRole::CONSTANT)
                .read(cluster_counts, ERGBufferRole::STORAGE)
                .write(cluster_counts, ERGBufferRole::STORAGE)
                .after("ClusterBuild")
                .setKernelFn([light_res](const PassRecordContext &rec)
                             {
                    const uint32_t point_count = light_res ? light_res->lightCount<PointLightGPU>() : 0u;
                    const uint32_t spot_count = light_res ? light_res->lightCount<SpotLightGPU>() : 0u;
                    const uint32_t total = point_count + spot_count;
                    if (total == 0u || rec.pipeline_layout == VK_NULL_HANDLE)
                        return;
                    vkCmdDispatch(rec.cmd, total, 1u, 1u); })
                .setKernel("ClusterCount");

            builder.addPass("PrefixScan", ERGPassType::COMPUTE)
                .setComputePipeline(cluster_scan_pipeline_)
                .bindTransientDS(0, cluster_tds)
                .read(cluster_params, ERGBufferRole::CONSTANT)
                .readWrite(cluster_counts, ERGBufferRole::STORAGE)
                .readWrite(cluster_offsets, ERGBufferRole::STORAGE)
                .readWrite(cluster_write_heads, ERGBufferRole::STORAGE)
                .readWrite(cluster_overflow, ERGBufferRole::STORAGE)
                .after("ClusterCount")
                .setKernelFn([](const PassRecordContext &rec)
                             { vkCmdDispatch(rec.cmd, 1u, 1u, 1u); })
                .setKernel("PrefixScan");

            builder.addPass("ClusterFill", ERGPassType::COMPUTE)
                .setComputePipeline(cluster_fill_pipeline_)
                .bindTransientDS(0, cluster_tds)
                .useEngineSet(EDescriptorSetSlot::Light,
                                builder.trackExternalBuffer("ext.LightResources"), ERGResourceType::BUFFER)
                .read(cluster_params, ERGBufferRole::CONSTANT)
                .read(cluster_offsets, ERGBufferRole::STORAGE)
                .readWrite(cluster_write_heads, ERGBufferRole::STORAGE)
                .readWrite(cluster_indices, ERGBufferRole::STORAGE)
                .readWrite(cluster_overflow, ERGBufferRole::STORAGE)
                .after("PrefixScan")
                // P1 of the local-read design (mobile deferred read path): the
                // cluster chain has NO data edge to the G-buffer, so pull it
                // ahead of the G-buffer draw. This makes GBufferDraw and
                // DeferredLighting topologically ADJACENT graphics passes — the
                // precondition for merging them into one local-read rendering
                // scope (P2). Pure reorder: zero visual change on any path.
                .before(kDeferredGBufferDrawPassName)
                .setKernelFn(
                    [light_res](const PassRecordContext &rec)
                    {
                        const uint32_t point_count = light_res ? light_res->lightCount<PointLightGPU>() : 0u;
                        const uint32_t spot_count = light_res ? light_res->lightCount<SpotLightGPU>() : 0u;
                        const uint32_t total = point_count + spot_count;
                        if (total == 0u || rec.pipeline_layout == VK_NULL_HANDLE)
                            return;
                        vkCmdDispatch(rec.cmd, total, 1u, 1u); 
                    }
                )
                .setKernel("ClusterFill");
        }

        auto lighting_pass = builder.addPass("DeferredLighting", ERGPassType::GRAPHICS);
        if (effective_local_read_)
        {
            // Local-read merged scope: declare tile-local input
            // reads. The planner absorbs this pass into the G-buffer group
            // (try_local_read_merge) — indices must bit-match the shader's
            // input_attachment_index decorations and the template lr_* maps.
            lighting_pass.inputRead(gbuf_albedo, 0)
                         .inputRead(gbuf_normal, 1)
                         .inputRead(gbuf_emissive, 2)
                         .inputRead(gbuf_depth, 3);
        }
        else
        {
            lighting_pass.read(gbuf_albedo, lux::common::ETextureRole::SAMPLED)
                         .read(gbuf_normal, lux::common::ETextureRole::SAMPLED)
                         .read(gbuf_emissive, lux::common::ETextureRole::SAMPLED)
                         .read(gbuf_depth, lux::common::ETextureRole::SAMPLED);
        }
        lighting_pass
                                 .read(cluster_params, ERGBufferRole::CONSTANT)
                                 .read(cluster_offsets, ERGBufferRole::STORAGE)
                                 .read(cluster_indices, ERGBufferRole::STORAGE)
                                 .read(cluster_overflow, ERGBufferRole::STORAGE)
                                 .write(lit_color, lux::common::ETextureRole::COLOR_ATTACHMENT)
                                 .setPipeline(lighting_pipeline_)
                                 .bindSceneDS()
                                 .bindTransientDS(1, gbuffer_tds)
                                 .useEngineSet(EDescriptorSetSlot::Light,
                                                 builder.trackExternalBuffer("ext.LightResources"), ERGResourceType::BUFFER)
                                 .bindTransientDS(3, cluster_tds);

        // Shadow atlas must be rendered before the lighting pass reads it via
        // the Light descriptor set (bindings 4-6).  Declaring a read dependency
        // ensures the render graph schedules MeshShadowDraw before this pass.
        lighting_pass.read(shadow_atlas, lux::common::ETextureRole::SAMPLED);
        lighting_pass.after(kShadowViewUploadPassName);
        // Light DS samples shadow atlas (bindings 4-6). Enforce explicit ordering
        // against the shadow draw pass to avoid sampling stale atlas contents.
        lighting_pass.after(kMeshShadowDrawPassName);
        lighting_pass.after("ClusterBuild");
        if (clustered_enabled)
            lighting_pass.after("ClusterFill");

        // EVSM: the lighting fragment samples the blurred moment atlas through
        // the Light descriptor set (bindings 4-6), which points at the raw EVSM
        // image view. The separable blur ping-pongs back into the moment image
        // (3 atlases → 2), so the final blurred result lives in
        // `evsm_moment_atlas`. That access is invisible to the render graph, so
        // without an explicit read dependency the graph neither orders
        // DeferredLighting after the vertical blur (EVSMBlurV) nor inserts the
        // GENERAL→SHADER_READ_ONLY layout transition + compute-write→fragment-read
        // memory barrier. On a single atlas shared across frames-in-flight that is
        // a read-while-write hazard: flickering, fragmented shadows, and (with
        // validation enabled) a per-frame flood of SYNC-HAZARD errors that tanks
        // FPS. Declaring the read makes the graph schedule + barrier it correctly.
        // (PCF mode keeps sampling the D32 `shadow_atlas` read declared above.)
        if (cfg_.technique == EShadowTechnique::EVSM)
        {
            lighting_pass.read(builder.referenceTexture("evsm_moment_atlas"),
                               lux::common::ETextureRole::SAMPLED);
            lighting_pass.after(kEvsmBlurVPassName);
        }

        lighting_pass.setKernelFn([](const PassRecordContext &rec)
                                  { vkCmdDraw(rec.cmd, 3, 1, 0, 0); });
        lighting_pass.setKernel("DeferredLighting");
    }

} // namespace lux::render
