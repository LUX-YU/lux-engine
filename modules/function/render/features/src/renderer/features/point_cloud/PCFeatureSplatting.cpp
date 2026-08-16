/**
 * @file PCFeatureSplatting.cpp
 * @brief Gaussian soft-splat point cloud feature — thin draw layer over PCFeatureIndirectBase.
 */

#include <lux/engine/render/renderer/features/point_cloud/PCFeatureSplatting.hpp>
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

PCFeatureSplatting::PCFeatureSplatting(Config cfg)
    : PCFeatureIndirectBase(cfg.compute_shader, cfg.max_nodes,
                            RenderFeature::Config{.name = "PointCloudSplatting"}, "Splat")
    , max_size_(cfg.max_size)
    , cfg_(std::move(cfg))
{
}

lux::render::Expected<void> PCFeatureSplatting::initAndAttachTo(RenderScene& scene){
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
        PipelineStageRequest{EBuiltinShader::PC_LOD_VERT, cfg_.vertex_shader},
        PipelineStageRequest{EBuiltinShader::PC_SPLAT_FRAG, cfg_.fragment_shader}};

    auto stages = preparePipelineStages(shaders, stage_requests);
    if (!stages)
        return lux::cxx::unexpected(stages.error());

    // Graphics pipeline: same depth-scaled vertex shader as LOD, Gaussian splat fragment.
    // Key differences from the LOD/GPUDriven pipeline:
    //   - blend_enable        = VK_TRUE   (splats are alpha-blended)
    //   - depth_write_enable  = VK_FALSE  (no depth write for translucent splats)
    auto tmpl = makePointCloudTemplate();
    tmpl.descriptor_set_count      = 1;
    tmpl.vertex_shader             = stages->module(0);
    tmpl.fragment_shader           = stages->module(1);
    tmpl.blend_enable              = VK_TRUE;
    tmpl.depth_write_enable        = VK_FALSE;
    tmpl.src_color_blend_factor    = VK_BLEND_FACTOR_SRC_ALPHA;
    tmpl.dst_color_blend_factor    = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    tmpl.color_blend_op            = VK_BLEND_OP_ADD;
    tmpl.src_alpha_blend_factor    = VK_BLEND_FACTOR_ONE;
    tmpl.dst_alpha_blend_factor    = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    tmpl.alpha_blend_op            = VK_BLEND_OP_ADD;
    // Layout left empty here; it's built via reflection instead — this
    // pipeline only uses the Scene set, and the contract routes it back to
    // the same shared engine layout, equivalent to building it by hand.
    tmpl.debug_name = "PointCloudSplat";
    auto pipeline = ctx.pipelineManager().registerGraphicsTemplate(tmpl, stages->infos());
    if (!pipeline)
        return lux::cxx::unexpected(pipeline.error());
    draw_handle_ = *pipeline;
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
                max_size_,
            };
            vkCmdPushConstants(ctx.cmd, ctx.pipeline_layout,
                               ctx.pc_stage_flags, kViewPushPrefixSize,
                               sizeof(LodPC), &lod_pc);
        });
}

} // namespace lux::render
