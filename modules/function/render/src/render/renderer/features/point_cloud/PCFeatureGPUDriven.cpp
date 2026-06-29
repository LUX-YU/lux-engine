/**
 * @file PCFeatureGPUDriven.cpp
 * @brief GPU-Driven point cloud feature — thin draw layer over PCFeatureIndirectBase.
 */

#include <lux/engine/render/renderer/features/point_cloud/PCFeatureGPUDriven.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PointCloudGpuData.hpp>
#include <lux/engine/render/resources/point_cloud/PointCloudGlobalBuffer.hpp>
#include <lux/engine/render/resources/point_cloud/GpuOctreeNodeBuffer.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>

#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/StandardPipelineLayoutBuilder.hpp>
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

void PCFeatureGPUDriven::initAndAttachTo(RenderScene& scene)
{
    PCFeatureIndirectBase::initAndAttachTo(scene);

    auto& ctx = renderContext();
    const auto& cfg = cfg_;
    auto* shaders = ctx.globalRegistry().find<ShaderResources>();
    cfg_.vertex_shader   = ensureBuiltinShader(shaders, cfg_.vertex_shader,   EBuiltinShader::PC_SIMPLE_VERT);
    cfg_.fragment_shader = ensureBuiltinShader(shaders, cfg_.fragment_shader, EBuiltinShader::PC_SIMPLE_FRAG);
    auto& vs = *shaders->get(cfg.vertex_shader);
    auto& fs = *shaders->get(cfg.fragment_shader);

    // Graphics pipeline: fixed screen-space point size, single scene descriptor set.
    auto tmpl = makePointCloudTemplate();
    tmpl.descriptor_set_count = 1;
    tmpl.vertex_shader   = vs.module;
    tmpl.fragment_shader = fs.module;

    std::vector<const lux::rdesc::ShaderInfo*> infos = {
        &vs.info, &fs.info};
    tmpl.pipeline_layout = buildStandardGraphicsPipelineLayout(
        ctx,
        tmpl.descriptor_set_count,
        infos,
        "PointCloudGPUDrivenLayout").value();
    draw_handle_ = ctx.pipelineManager().registerGraphicsTemplate(tmpl, infos).value();
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
            const float point_size = point_size_.load(std::memory_order_relaxed);
            vkCmdPushConstants(ctx.cmd, ctx.pipeline_layout,
                               ctx.pc_stage_flags, 8,
                               sizeof(float), &point_size);
        });
}

} // namespace lux::render
