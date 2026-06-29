/**
 * @file PCFeatureLOD.cpp
 * @brief Depth-scaled LOD point cloud feature — thin draw layer over PCFeatureIndirectBase.
 */

#include <lux/engine/render/renderer/features/point_cloud/PCFeatureLOD.hpp>
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

PCFeatureLOD::PCFeatureLOD(Config cfg)
    : PCFeatureIndirectBase(cfg.compute_shader, cfg.max_nodes,
                            RenderFeature::Config{.name = "PointCloudLOD"}, "LOD")
    , max_size_atomic_(cfg.max_size)
    , cfg_(std::move(cfg))
{
}

void PCFeatureLOD::initAndAttachTo(RenderScene& scene)
{
    PCFeatureIndirectBase::initAndAttachTo(scene);

    auto& ctx = renderContext();
    const auto& cfg = cfg_;
    auto* shaders = ctx.globalRegistry().find<ShaderResources>();
    cfg_.vertex_shader   = ensureBuiltinShader(shaders, cfg_.vertex_shader,   EBuiltinShader::PC_LOD_VERT);
    cfg_.fragment_shader = ensureBuiltinShader(shaders, cfg_.fragment_shader, EBuiltinShader::PC_SIMPLE_FRAG);
    auto& vs = *shaders->get(cfg.vertex_shader);
    auto& fs = *shaders->get(cfg.fragment_shader);

    // Graphics pipeline: same point-list topology, LOD vertex shader computes
    // perspective-correct gl_PointSize from clip.w and push-constant world radius.
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
        "PointCloudLODLayout").value();
    draw_handle_ = ctx.pipelineManager().registerGraphicsTemplate(tmpl, infos).value();
}

// ============================================================================
//  RenderFeature — addPasses
// ============================================================================

void PCFeatureLOD::addPasses(RGBuilder& builder)
{
    if (!node_buf_)
        return;

    // --- Compute: frustum culling + indirect buffer generation (base helper) ---
    RGResourceHandle inst_rg, indirect_rg;
    addComputePass(builder, inst_rg, indirect_rg);

    // --- Graphics: single indirect draw with depth-scaled point size (base helper) ---
    addIndirectDrawPass(builder, "PCLODDraw", cfg_.color_target, cfg_.depth_target, indirect_rg,
        [this](const PassRecordContext& ctx)
        {
            // push constant: LodPC {point_size_world, min_size, max_size} (12 bytes, offset 8)
            const LodPC lod_pc{
                cfg_.point_size_world,
                cfg_.min_size,
                max_size_atomic_.load(std::memory_order_relaxed),
            };
            vkCmdPushConstants(ctx.cmd, ctx.pipeline_layout,
                               ctx.pc_stage_flags, 8,
                               sizeof(LodPC), &lod_pc);
        });
}

} // namespace lux::render
