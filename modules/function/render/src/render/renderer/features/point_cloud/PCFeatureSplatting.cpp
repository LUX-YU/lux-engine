/**
 * @file PCFeatureSplatting.cpp
 * @brief Gaussian soft-splat point cloud feature — thin draw layer over PCFeatureIndirectBase.
 */

#include <lux/engine/render/renderer/features/point_cloud/PCFeatureSplatting.hpp>
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

PCFeatureSplatting::PCFeatureSplatting(Config cfg)
    : PCFeatureIndirectBase(cfg.compute_shader, cfg.max_nodes,
                            RenderFeature::Config{.name = "PointCloudSplatting"}, "Splat")
    , max_size_atomic_(cfg.max_size)
    , cfg_(std::move(cfg))
{
}

lux::render::Expected<void> PCFeatureSplatting::initAndAttachTo(RenderScene& scene){
    PCFeatureIndirectBase::initAndAttachTo(scene);

    auto& ctx = renderContext();
    const auto& cfg = cfg_;
    auto* shaders = ctx.globalRegistry().find<ShaderResources>();
    cfg_.vertex_shader   = ensureBuiltinShader(shaders, cfg_.vertex_shader,   EBuiltinShader::PC_LOD_VERT);
    cfg_.fragment_shader = ensureBuiltinShader(shaders, cfg_.fragment_shader, EBuiltinShader::PC_SPLAT_FRAG);
    auto& vs = *shaders->get(cfg.vertex_shader);
    auto& fs = *shaders->get(cfg.fragment_shader);

    // Graphics pipeline: same depth-scaled vertex shader as LOD, Gaussian splat fragment.
    // Key differences from the LOD/GPUDriven pipeline:
    //   - blend_enable        = VK_TRUE   (splats are alpha-blended)
    //   - depth_write_enable  = VK_FALSE  (no depth write for translucent splats)
    auto tmpl = makePointCloudTemplate();
    tmpl.descriptor_set_count      = 1;
    tmpl.vertex_shader             = vs.module;
    tmpl.fragment_shader           = fs.module;
    tmpl.blend_enable              = VK_TRUE;
    tmpl.depth_write_enable        = VK_FALSE;
    tmpl.src_color_blend_factor    = VK_BLEND_FACTOR_SRC_ALPHA;
    tmpl.dst_color_blend_factor    = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    tmpl.color_blend_op            = VK_BLEND_OP_ADD;
    tmpl.src_alpha_blend_factor    = VK_BLEND_FACTOR_ONE;
    tmpl.dst_alpha_blend_factor    = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    tmpl.alpha_blend_op            = VK_BLEND_OP_ADD;

    std::vector<const lux::rdesc::ShaderInfo*> infos = {
        &vs.info, &fs.info};
    tmpl.pipeline_layout = buildStandardGraphicsPipelineLayout(
        ctx,
        tmpl.descriptor_set_count,
        infos,
        "PointCloudSplatLayout").value();
    draw_handle_ = ctx.pipelineManager().registerGraphicsTemplate(tmpl, infos).value();
    return {};
    }

// ============================================================================
//  RenderFeature — addPasses
// ============================================================================

void PCFeatureSplatting::addPasses(RGBuilder& builder)
{
    if (!node_buf_)
        return;

    // --- Compute: frustum culling + indirect buffer generation (base helper) ---
    RGResourceHandle inst_rg, indirect_rg;
    addComputePass(builder, inst_rg, indirect_rg);

    // --- Graphics: alpha-blended Gaussian splats (base helper) ---
    addIndirectDrawPass(builder, "PCSplatDraw", cfg_.color_target, cfg_.depth_target, indirect_rg,
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
