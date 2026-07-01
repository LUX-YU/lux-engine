/**
 * @file PCFeatureSimple.cpp
 * @brief Simple point cloud rendering feature (FramePack patch upload).
 */

#include <lux/engine/render/renderer/features/point_cloud/PCFeatureSimple.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PointCloudGpuData.hpp>
#include <lux/engine/render/resources/point_cloud/PointCloudGlobalBuffer.hpp>

#include <lux/engine/render/resources/PointCloudResources.hpp>
#include <lux/engine/render/transfer/TransferContributor.hpp>   // makeTransferContributor
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/renderer/RenderObjectExtraction.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PointCloudPipelinePreset.hpp>  // makePointCloudTemplate (+ GraphicsPipelineTemplate, vulkan.h)

namespace lux::render
{

PCFeatureSimple::PCFeatureSimple(Config cfg)
    : IPointCloudFeature(RenderFeature::Config{.name = "PointCloudSimple"})
    , point_size_(cfg.initial_point_size)
    , cfg_(std::move(cfg))
{
}

lux::render::Expected<void> PCFeatureSimple::initAndAttachTo(RenderScene& /*scene*/){
    // Self-contained feature: only the narrow RenderContextView / RenderSceneView
    // SDK surface — no engine-internal RenderContext / RenderScene / ShaderResources.
    auto cv = contextView();
    cfg_.vertex_shader   = cv.loadBuiltinShader(EBuiltinShader::PC_SIMPLE_VERT, cfg_.vertex_shader);
    cfg_.fragment_shader = cv.loadBuiltinShader(EBuiltinShader::PC_SIMPLE_FRAG, cfg_.fragment_shader);
    const auto& cfg = cfg_;

    // ---- Pipeline ----
    auto tmpl = makePointCloudTemplate();
    tmpl.descriptor_set_count = 1;
    tmpl.vertex_shader   = cv.shaderModule(cfg.vertex_shader);
    tmpl.fragment_shader = cv.shaderModule(cfg.fragment_shader);

    std::vector<const lux::rdesc::ShaderInfo*> infos = {
        cv.shaderInfo(cfg.vertex_shader), cv.shaderInfo(cfg.fragment_shader)};
    tmpl.pipeline_layout = cv.buildStandardGraphicsLayout(
        tmpl.descriptor_set_count,
        infos,
        "PointCloudSimpleLayout");
    pipeline_handle_ = cv.registerGraphics(tmpl, infos);

    // ---- Shared GPU resources (per-scene, lazy registration) ----
    auto sv = sceneView();
    auto* pc_res = sv.sceneRegistry().ensure<PointCloudResources>();
    pc_res->setDeferredQueue(&cv.deferredDestroyQueue());
    pc_res->setRetireScheduler(&cv.retireScheduler());
    pc_res->setRetireOwnerToken(sv.retireOwnerToken());
    if (!pc_res->isInitialized()) {
        pc_res->init(cv.vmaAllocator(), cfg.max_global_points, cfg.max_octree_nodes);
    }
    // Register PointCloudResources as a transfer contributor once (idempotent:
    // shared across PC features; only the first attach adds it). The scene core
    // used to do this lazily in recordUploads — the OWNER does it now.
    if (!pc_res->usesTransferScheduler()) {
        sv.transferScheduler().contributors().add(
            makeTransferContributor(pc_res, /*priority=*/1));
        pc_res->setUseTransferScheduler(true);
    }
    global_buf_ = &pc_res->globalBuffer();
    return {};
    }

void PCFeatureSimple::extractRenderObjects(
    const RenderObjectExtractContext& ctx,
    std::vector<DrawPacket>& out_packets)
{
    (void)ctx;
    (void)out_packets;
}

// ============================================================================
//  RenderFeature
// ============================================================================

void PCFeatureSimple::addPasses(RGBuilder& builder)
{
    builder.addPass("ForwardPointCloudSimple", ERGPassType::GRAPHICS)
        .write(builder.referenceTexture(cfg_.color_target), lux::common::ETextureRole::COLOR_ATTACHMENT)
        .write(builder.referenceTexture(cfg_.depth_target), lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
        .setPipeline(pipeline_handle_)
        .bindSceneDS(0)
        .setPhaseMask(phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::PointCloud)))
        .setKernelFn([this](const PassRecordContext& ctx)
        {
            if (!global_buf_ || global_buf_->buffer() == VK_NULL_HANDLE) return;
            if (ctx.view == nullptr) return;

            // Push point_size constant to the vertex shader.
            const float point_size = point_size_.load(std::memory_order_relaxed);
            vkCmdPushConstants(ctx.cmd, ctx.pipeline_layout,
                               ctx.pc_stage_flags, 8,
                               sizeof(float), &point_size);

            // Bind VB once at offset 0; use firstVertex to select per-slot data.
            VkBuffer vbuf = global_buf_->buffer();
            VkDeviceSize zero_offset = 0;
            vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vbuf, &zero_offset);

            global_buf_->forEachSlot(
                [&](uint32_t /*chunk_id*/, const PointCloudGlobalBuffer::Slot& slot)
            {
                if (slot.point_count == 0)
                    return;
                vkCmdDraw(ctx.cmd, slot.point_count, 1, slot.first_point, 0);
            });
        })
        .setKernel("PointCloudDraw");
}

void PCFeatureSimple::onFrameBegin(const FeatureFrameContext& ctx)
{
    // Nothing to do frame-to-frame: the transfer agent drains pending patches.
    (void)ctx;
}

} // namespace lux::render
