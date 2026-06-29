#include <lux/engine/render/renderer/features/deffer/DeferredLightingFeature.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/resources/lighting/LightResources.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraResource.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/core/VulkanCheck.hpp>
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

    void DeferredLightingFeature::initAndAttachTo(RenderScene & /*scene*/)
    {
        init();
    }

    void DeferredLightingFeature::onDetachFromScene(RenderScene & /*scene*/)
    {
        // gbuffer_sampler_ (FifOwned<VkSampler>) retires itself through the FIF
        // deferred-destroy queue when the feature is destroyed, so the runtime
        // removeFeature UAF (#17 sibling) is closed by construction — no manual
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
        auto *light_res = renderScene().sceneRegistry().find<LightResources>();
        const uint32_t light_count =
            light_res ? (light_res->lightCount<PointLightGPU>()
                       + light_res->lightCount<SpotLightGPU>())
                      : 0u;
        if (clusteringWanted(light_count) != clustering_compiled_)
            renderScene().invalidateGraph();
    }

    // =========================================================================
    //  Initialisation
    // =========================================================================

    void DeferredLightingFeature::init()
    {
        auto &ctx = renderContext();
        VkDevice device = ctx.deviceContext().logicalDevice();
        auto *shaders = ctx.globalRegistry().find<ShaderResources>();

        // ---- Ensure builtin shader defaults ----
        cfg_.vertex_shader        = ensureBuiltinShader(shaders, cfg_.vertex_shader,        EBuiltinShader::DEFERRED_LIGHTING_VERT);
        // Pick the SPIR-V variant matching the requested shadow technique.
        // The user may override `fragment_shader` explicitly — in that case
        // ensureBuiltinShader leaves the override untouched.
        const auto frag_variant = (cfg_.technique == EShadowTechnique::EVSM)
            ? EBuiltinShader::DEFERRED_LIGHTING_FRAG_EVSM
            : EBuiltinShader::DEFERRED_LIGHTING_FRAG_PCF;
        cfg_.fragment_shader      = ensureBuiltinShader(shaders, cfg_.fragment_shader,      frag_variant);
        cfg_.cluster_build_shader = ensureBuiltinShader(shaders, cfg_.cluster_build_shader, EBuiltinShader::CLUSTER_BUILD_COMP);
        cfg_.cluster_count_shader = ensureBuiltinShader(shaders, cfg_.cluster_count_shader, EBuiltinShader::CLUSTER_COUNT_COMP);
        cfg_.cluster_scan_shader  = ensureBuiltinShader(shaders, cfg_.cluster_scan_shader,  EBuiltinShader::CLUSTER_SCAN_COMP);
        cfg_.cluster_fill_shader  = ensureBuiltinShader(shaders, cfg_.cluster_fill_shader,  EBuiltinShader::CLUSTER_FILL_COMP);
        cfg_.cluster_clear_shader = ensureBuiltinShader(shaders, cfg_.cluster_clear_shader, EBuiltinShader::CLEAR_COUNT_BUFFERS_COMP);

        auto &vs = *shaders->get(cfg_.vertex_shader);
        auto &fs = *shaders->get(cfg_.fragment_shader);

        // ---- GBuffer descriptor set layout (4× combined_image_sampler) ----
        {
            const std::array<VkDescriptorSetLayoutBinding, 4> bindings{{
                {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            }};

            auto id = ctx.descriptorService().registerLayout({.bindings = bindings,
                                                              .debug_name = "GBufferDSLayout"});
            gbuffer_ds_layout_ = ctx.descriptorService().layout(id);
        }

        // ---- Clustered deferred descriptor set layout ----
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

        // ---- Counter clear descriptor set layout (8× storage buffer) ----
        {
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

            auto id = ctx.descriptorService().registerLayout({.bindings = bindings,
                                                              .debug_name = "ClusterClearDSLayout"});
            cluster_clear_ds_layout_ = ctx.descriptorService().layout(id);
        }

        // ---- Nearest-clamp sampler for GBuffer textures ----
        {
            VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            si.magFilter = VK_FILTER_NEAREST;
            si.minFilter = VK_FILTER_NEAREST;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.maxLod = 0.0f;
            VkSampler sampler = VK_NULL_HANDLE;
            VK_CHECK(vkCreateSampler(device, &si, nullptr, &sampler));
            gbuffer_sampler_ = FifOwned<VkSampler>{&ctx.deferredDestroyQueue(), sampler};
        }

        // ---- Fullscreen lighting pipeline ----
        //
        // Non-standard DS layout: Set 0 = Scene, Set 1 = GBuffer, Set 2 = Light+Shadow
        // We build the pipeline layout manually and set resource_slot_map so the
        // framework knows how to bind the Scene and Light DS at the correct slots.
        {
            GraphicsPipelineTemplate tmpl{};
            tmpl.geometry_type = EGeometryType::MESH;
            tmpl.vertex_shader = vs.module;
            tmpl.fragment_shader = fs.module;
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

            const VkPushConstantRange pc{
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
            const std::array set_layouts{
                ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Scene), // set 0
                gbuffer_ds_layout_,                                           // set 1
                ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Light), // set 2
                cluster_ds_layout_,                                           // set 3
            };
            const std::array pcs{pc};
            tmpl.pipeline_layout = ctx.pipelineLayoutService().getOrCreate({.set_layouts = set_layouts,
                                                                            .push_constants = pcs,
                                                                            .debug_name = "DeferredLightingLayout"})
                                       .value();
            // Authoritative PC ranges from the layout — prevents reflection
            // from dropping VERTEX_BIT (fullscreen vertex shader has no PC block).
            tmpl.push_constant_ranges.assign(pcs.begin(), pcs.end());

            std::vector<const rdesc::ShaderInfo *> infos = {
                &vs.info,
                &fs.info};
            lighting_pipeline_ = ctx.pipelineManager().registerGraphicsTemplate(tmpl, infos).value();
        }

        auto create_cluster_pipeline = [&](ShaderHandle shader_handle, const char *debug_name) -> ComputePipelineHandle
        {
            if (shader_handle.is_null())
                return kInvalidComputePipelineHandle;

            auto *shader_res = ctx.globalRegistry().find<ShaderResources>();
            auto *shader_obj = shader_res ? shader_res->get(shader_handle) : nullptr;
            if (!shader_obj)
                return kInvalidComputePipelineHandle;

            const std::array set_layouts{
                cluster_ds_layout_,
                ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Light),
            };
            const VkPipelineLayout layout = ctx.pipelineLayoutService().getOrCreate({
                                                                                        .set_layouts = set_layouts,
                                                                                        .push_constants = std::span<const VkPushConstantRange>{},
                                                                                        .debug_name = debug_name,
                                                                                    })
                                                .value();
            return ctx.pipelineManager().registerComputePipeline(shader_obj->module, layout);
        };

        auto create_clear_pipeline = [&](ShaderHandle shader_handle, const char *debug_name) -> ComputePipelineHandle
        {
            if (shader_handle.is_null())
                return kInvalidComputePipelineHandle;

            auto *shader_res = ctx.globalRegistry().find<ShaderResources>();
            auto *shader_obj = shader_res ? shader_res->get(shader_handle) : nullptr;
            if (!shader_obj)
                return kInvalidComputePipelineHandle;

            const VkPushConstantRange pc{
                VK_SHADER_STAGE_COMPUTE_BIT,
                0,
                static_cast<uint32_t>(sizeof(uint32_t) * 10u)};
            const std::array set_layouts{cluster_clear_ds_layout_};
            const std::array pcs{pc};
            const VkPipelineLayout layout = ctx.pipelineLayoutService().getOrCreate({
                                                                                        .set_layouts = set_layouts,
                                                                                        .push_constants = pcs,
                                                                                        .debug_name = debug_name,
                                                                                    })
                                                .value();
            return ctx.pipelineManager().registerComputePipeline(shader_obj->module, layout);
        };

        cluster_build_pipeline_ = create_cluster_pipeline(cfg_.cluster_build_shader, "ClusterBuildLayout");
        cluster_count_pipeline_ = create_cluster_pipeline(cfg_.cluster_count_shader, "ClusterCountLayout");
        cluster_scan_pipeline_ = create_cluster_pipeline(cfg_.cluster_scan_shader, "ClusterScanLayout");
        cluster_fill_pipeline_ = create_cluster_pipeline(cfg_.cluster_fill_shader, "ClusterFillLayout");
        cluster_clear_pipeline_ = create_clear_pipeline(cfg_.cluster_clear_shader, "ClusterClearCountersLayout");
    }

    void DeferredLightingFeature::destroy() noexcept
    {
        // gbuffer_sampler_ (FifOwned) retires through the deferred-destroy queue
        // when this feature is destroyed — no inline vkDestroySampler. (C1)
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
        auto gbuffer_tds = builder.createTransientDS("GBufferDS", gbuffer_ds_layout_, {
                                                                                          {0, EDescriptorType::COMBINED_IMAGE_SAMPLER, gbuf_albedo, gbuffer_sampler_.get(), EImageLayout::SHADER_READ_ONLY_OPTIMAL},
                                                                                          {1, EDescriptorType::COMBINED_IMAGE_SAMPLER, gbuf_normal, gbuffer_sampler_.get(), EImageLayout::SHADER_READ_ONLY_OPTIMAL},
                                                                                          {2, EDescriptorType::COMBINED_IMAGE_SAMPLER, gbuf_emissive, gbuffer_sampler_.get(), EImageLayout::SHADER_READ_ONLY_OPTIMAL},
                                                                                          {3, EDescriptorType::COMBINED_IMAGE_SAMPLER, gbuf_depth, gbuffer_sampler_.get(), EImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                                                                                      });

        // ---- Reference shadow atlas (forward ref — if ShadowMapFeature absent, pass is pruned) ----
        auto shadow_atlas = builder.referenceTexture(cfg_.shadow_atlas);

        auto *light_res = renderScene().sceneRegistry().find<LightResources>();

        const uint32_t cluster_x = std::max(cfg_.cluster_x, 1u);
        const uint32_t cluster_y = std::max(cfg_.cluster_y, 1u);
        const uint32_t cluster_z = std::max(cfg_.cluster_z, 1u);
        const uint32_t cluster_count = cluster_x * cluster_y * cluster_z;

        struct alignas(16) ClusterParamsGPU
        {
            float view[16];
            float proj[16];
            float viewport[4];  // width, height, near, far
            uint32_t grid[4];   // x, y, z, enable_clustered
            uint32_t limits[4]; // cluster_count, max_indices, point_count, spot_count
        };

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
                    auto* cam = renderScene().sceneRegistry().find<ViewCameraResource>();
                    const ViewFrameData* cam_fd = cam ? cam->find(rec.view->handle) : nullptr;
                    ViewFrameData vfd = cam_fd ? *cam_fd : ViewFrameData{};
                    const auto& cv = vfd.camera_view;
                    std::memcpy(params.view, cv.view.data(), sizeof(params.view));
                    std::memcpy(params.proj, cv.proj.data(), sizeof(params.proj));
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

                vkCmdUpdateBuffer(rec.cmd, params_buf, 0, sizeof(ClusterParamsGPU), &params);

                if (!clear_counters_enabled)
                {
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
                .bindResourceDS(1, light_res, &LightResources::resolveDS, EDSBindMode::PER_FIF,
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
                .bindResourceDS(1, light_res, &LightResources::resolveDS, EDSBindMode::PER_FIF,
                                builder.trackExternalBuffer("ext.LightResources"), ERGResourceType::BUFFER)
                .read(cluster_params, ERGBufferRole::CONSTANT)
                .read(cluster_offsets, ERGBufferRole::STORAGE)
                .readWrite(cluster_write_heads, ERGBufferRole::STORAGE)
                .readWrite(cluster_indices, ERGBufferRole::STORAGE)
                .readWrite(cluster_overflow, ERGBufferRole::STORAGE)
                .after("PrefixScan")
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

        auto lighting_pass = builder.addPass("DeferredLighting", ERGPassType::GRAPHICS)
                                 .read(gbuf_albedo, lux::common::ETextureRole::SAMPLED)
                                 .read(gbuf_normal, lux::common::ETextureRole::SAMPLED)
                                 .read(gbuf_emissive, lux::common::ETextureRole::SAMPLED)
                                 .read(gbuf_depth, lux::common::ETextureRole::SAMPLED)
                                 .read(cluster_params, ERGBufferRole::CONSTANT)
                                 .read(cluster_offsets, ERGBufferRole::STORAGE)
                                 .read(cluster_indices, ERGBufferRole::STORAGE)
                                 .read(cluster_overflow, ERGBufferRole::STORAGE)
                                 .write(lit_color, lux::common::ETextureRole::COLOR_ATTACHMENT)
                                 .setPipeline(lighting_pipeline_)
                                 .bindSceneDS(0)
                                 .bindTransientDS(1, gbuffer_tds)
                                 .bindResourceDS(2, light_res, &LightResources::resolveDS, EDSBindMode::PER_FIF,
                                                 builder.trackExternalBuffer("ext.LightResources"), ERGResourceType::BUFFER)
                                 .bindTransientDS(3, cluster_tds);

        // Shadow atlas must be rendered before the lighting pass reads it via
        // the Light descriptor set (bindings 4-6).  Declaring a read dependency
        // ensures the render graph schedules MeshShadowDraw before this pass.
        lighting_pass.read(shadow_atlas, lux::common::ETextureRole::SAMPLED);
        lighting_pass.after("ShadowViewUpload");
        // Light DS samples shadow atlas (bindings 4-6). Enforce explicit ordering
        // against the shadow draw pass to avoid sampling stale atlas contents.
        lighting_pass.after("MeshShadowDraw");
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
            lighting_pass.after("EVSMBlurV");
        }

        lighting_pass.setKernelFn([](const PassRecordContext &rec)
                                  { vkCmdDraw(rec.cmd, 3, 1, 0, 0); });
        lighting_pass.setKernel("DeferredLighting");
    }

} // namespace lux::render
