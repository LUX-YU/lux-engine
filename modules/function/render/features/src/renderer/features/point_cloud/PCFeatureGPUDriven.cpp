/**
 * @file PCFeatureGPUDriven.cpp
 * @brief GPU-Driven point cloud feature — thin draw layer over PCFeatureIndirectBase.
 */

#include <lux/engine/render/renderer/features/point_cloud/PCFeatureGPUDriven.hpp>
#include <lux/engine/render/resources/point_cloud/PointCloudGpuData.hpp>
#include <lux/engine/render/resources/point_cloud/PointCloudGlobalBuffer.hpp>
#include <lux/engine/render/resources/point_cloud/GpuOctreeNodeBuffer.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>

#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/StandardPipelineLayoutBuilder.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PointCloudPipelinePreset.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>


namespace lux::render
{

// ============================================================================
//  Constructor
// ============================================================================

PCFeatureGPUDriven::PCFeatureGPUDriven(Config cfg)
    : PCFeatureIndirectBase(cfg.compute_shader, cfg.max_nodes,
                            RenderFeature::Config{.name = "PointCloudGPUDriven"}, "GPUDriven")
    , point_size_(cfg.initial_point_size)
    , cfg_(std::move(cfg))
{
}

lux::render::Expected<void> PCFeatureGPUDriven::initAndAttachTo(RenderScene& scene){
    // 基类 attach 失败即整体失败 —— 它建的是本特性绘制所依赖的间接绘制机件,
    // 少了它这个特性装上也画不出东西。
    if (auto base = PCFeatureIndirectBase::initAndAttachTo(scene); !base)
        return base;

    auto& ctx = renderContext();
    const auto& cfg = cfg_;
    auto& shaders = ctx.globalRegistry().must<ShaderResources>();
    // 解析内置默认 + 域合并切换 + 取模块反射,一次做完。引用引擎契约资源(本 vert
    // 用 uViews)的管线必须带域合并标记,否则 PipelineManager 拒绝注册。
    const std::array stage_requests{
        PipelineStageRequest{EBuiltinShader::PC_SIMPLE_VERT, cfg_.vertex_shader},
        PipelineStageRequest{EBuiltinShader::PC_SIMPLE_FRAG, cfg_.fragment_shader}};

    auto stages = preparePipelineStages(shaders, stage_requests);
    if (!stages)
        return lux::cxx::unexpected(stages.error());

    // Graphics pipeline: fixed screen-space point size, single scene descriptor set.
    auto tmpl = makePointCloudTemplate();
    tmpl.descriptor_set_count = 1;
    tmpl.vertex_shader   = stages->module(0);
    tmpl.fragment_shader = stages->module(1);
    // Layout left empty here; it's built via reflection instead — this
    // pipeline only uses the Scene set, and the contract routes it back to
    // the same shared engine layout, equivalent to building it by hand.
    tmpl.debug_name = "PointCloudGPUDriven";
    auto pipeline = ctx.pipelineManager().registerGraphicsTemplate(tmpl, stages->infos());
    if (!pipeline)
        return lux::cxx::unexpected(pipeline.error());
    draw_handle_ = *pipeline;
    return {};
    }

// ============================================================================
//  RenderFeature — addPasses
// ============================================================================

void PCFeatureGPUDriven::addPasses(RGBuilder& builder)
{
    if (!node_buf_)
        return;

    // --- Compute: frustum culling + indirect buffer generation (base helper) ---
    RGResourceHandle inst_rg, indirect_rg;
    addComputePass(builder, inst_rg, indirect_rg);

    // --- Graphics: single indirect draw of all visible points (base helper) ---
    addIndirectDrawPass(builder, "PCGPUDrivenDraw", cfg_.color_target, cfg_.depth_target, indirect_rg,
        [this](const PassRecordContext& ctx)
        {
            // push constant: float point_size (offset 8, VK_SHADER_STAGE_VERTEX_BIT)
            const float point_size = point_size_;
            vkCmdPushConstants(ctx.cmd, ctx.pipeline_layout,
                               ctx.pc_stage_flags, kViewPushPrefixSize,
                               sizeof(float), &point_size);
        });
}

} // namespace lux::render
