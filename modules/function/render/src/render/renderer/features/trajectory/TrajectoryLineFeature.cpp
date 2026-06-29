/**
 * @file TrajectoryLineFeature.cpp
 * @brief Simple trajectory line rendering feature (LINE_STRIP draw per slot).
 */

#include <lux/engine/render/renderer/features/trajectory/TrajectoryLineFeature.hpp>
#include <lux/engine/render/renderer/features/trajectory/TrajectoryGpuData.hpp>
#include <lux/engine/render/resources/TrajectoryGlobalBuffer.hpp>

#include <lux/engine/render/resources/TrajectoryResources.hpp>
#include <lux/engine/render/transfer/TransferContributor.hpp>   // makeTransferContributor
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/renderer/RenderObjectExtraction.hpp>
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>  // GraphicsPipelineTemplate (+ vulkan.h)

namespace lux::render
{

namespace
{
    /// Trajectory line preset (LINE_STRIP) — GpuTrajectoryVertex layout. Feature-
    /// local (not in pipeline/PipelinePresets.hpp): it hard-codes a trajectory-
    /// domain vertex type, so the generic pipeline layer stays domain-free.
    GraphicsPipelineTemplate makeTrajectoryLineTemplate()
    {
        GraphicsPipelineTemplate tmpl{};

        VkVertexInputBindingDescription b{};
        b.binding   = 0;
        b.stride    = static_cast<uint32_t>(sizeof(GpuTrajectoryVertex));
        b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        tmpl.vertex_bindings = { b };

        tmpl.vertex_attributes = {
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuTrajectoryVertex, x)},
            VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32_UINT,          offsetof(GpuTrajectoryVertex, packed_color)},
            VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32_SFLOAT,        offsetof(GpuTrajectoryVertex, time)},
            VkVertexInputAttributeDescription{3, 0, VK_FORMAT_R32_SFLOAT,        offsetof(GpuTrajectoryVertex, width)},
        };

        tmpl.topology           = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        tmpl.polygon_mode       = VK_POLYGON_MODE_FILL;
        tmpl.cull_mode          = VK_CULL_MODE_NONE;
        tmpl.line_width         = 3.0f;
        tmpl.front_face         = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        tmpl.depth_test_enable   = VK_TRUE;
        tmpl.depth_write_enable  = VK_FALSE;
        tmpl.depth_compare_op    = VK_COMPARE_OP_LESS_OR_EQUAL;
        tmpl.blend_enable        = VK_TRUE;
        tmpl.src_color_blend_factor = VK_BLEND_FACTOR_SRC_ALPHA;
        tmpl.dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        tmpl.color_blend_op      = VK_BLEND_OP_ADD;
        tmpl.src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
        tmpl.dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        tmpl.alpha_blend_op      = VK_BLEND_OP_ADD;
        tmpl.color_write_mask    = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        tmpl.use_dynamic_viewport = true;
        tmpl.use_dynamic_scissor  = true;
        return tmpl;
    }
} // namespace

TrajectoryLineFeature::TrajectoryLineFeature(Config cfg)
    : ITrajectoryFeature(RenderFeature::Config{.name = "TrajectoryLine"})
    , cfg_(std::move(cfg))
{
}

void TrajectoryLineFeature::initAndAttachTo(RenderScene& /*scene*/)
{
    // Self-contained feature: only the narrow RenderContextView / RenderSceneView
    // SDK surface — no engine-internal RenderContext / RenderScene / ShaderResources.
    auto cv = contextView();
    cfg_.vertex_shader   = cv.loadBuiltinShader(EBuiltinShader::TRAJECTORY_LINE_VERT, cfg_.vertex_shader);
    cfg_.fragment_shader = cv.loadBuiltinShader(EBuiltinShader::TRAJECTORY_LINE_FRAG, cfg_.fragment_shader);
    const auto& cfg = cfg_;

    // ---- Pipeline ----
    auto tmpl = makeTrajectoryLineTemplate();
    tmpl.descriptor_set_count = 1;
    tmpl.vertex_shader   = cv.shaderModule(cfg.vertex_shader);
    tmpl.fragment_shader = cv.shaderModule(cfg.fragment_shader);

    std::vector<const lux::rdesc::ShaderInfo*> infos = {
        cv.shaderInfo(cfg.vertex_shader), cv.shaderInfo(cfg.fragment_shader)};
    tmpl.pipeline_layout = cv.buildStandardGraphicsLayout(
        tmpl.descriptor_set_count,
        infos,
        "TrajectoryLineLayout");
    pipeline_handle_ = cv.registerGraphics(tmpl, infos);

    // ---- Shared GPU resources (per-scene, lazy registration) ----
    auto sv = sceneView();
    auto* traj_res = sv.sceneRegistry().ensure<TrajectoryResources>();
    traj_res->setDeferredQueue(&cv.deferredDestroyQueue());
    traj_res->setRetireScheduler(&cv.retireScheduler());
    traj_res->setRetireOwnerToken(sv.retireOwnerToken());
    if (!traj_res->isInitialized()) {
        traj_res->init(cv.vmaAllocator(), cfg.max_global_vertices);
    }
    // Register TrajectoryResources as a transfer contributor once (idempotent).
    // The scene core used to do this lazily in recordUploads — the OWNER does it
    // now, keeping the core scene domain-free.
    if (!traj_res->usesTransferScheduler()) {
        sv.transferScheduler().contributors().add(
            makeTransferContributor(traj_res, /*priority=*/2));
        traj_res->setUseTransferScheduler(true);
    }
    global_buf_ = &traj_res->globalBuffer();
}

void TrajectoryLineFeature::extractRenderObjects(
    const RenderObjectExtractContext& ctx,
    std::vector<DrawPacket>& out_packets)
{
    (void)ctx;
    (void)out_packets;
}

// ============================================================================
//  RenderFeature
// ============================================================================

void TrajectoryLineFeature::addPasses(RGBuilder& builder)
{
    builder.addPass("ForwardTrajectoryLine", ERGPassType::GRAPHICS)
        .write(builder.referenceTexture(cfg_.color_target), lux::common::ETextureRole::COLOR_ATTACHMENT)
        .write(builder.referenceTexture(cfg_.depth_target), lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
        .setPipeline(pipeline_handle_)
        .bindSceneDS(0)
        .setPhaseMask(phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::ForwardTrans)))
        .setKernelFn([this](const PassRecordContext& ctx)
        {
            if (!global_buf_ || global_buf_->buffer() == VK_NULL_HANDLE) return;
            if (ctx.view == nullptr) return;

            // Bind the unified trajectory vertex buffer once at offset 0.
            VkBuffer vbuf = global_buf_->buffer();
            VkDeviceSize zero_offset = 0;
            vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vbuf, &zero_offset);

            global_buf_->forEachTrajectory(
                [&](uint32_t /*trajectory_id*/, const TrajectoryGlobalBuffer::Slot& slot)
            {
                if (slot.vertex_count == 0)
                    return;
                vkCmdDraw(ctx.cmd, slot.vertex_count, 1, slot.first_vertex, 0);
            });
        })
        .setKernel("TrajectoryDraw");
}

void TrajectoryLineFeature::onFrameBegin(const FeatureFrameContext& ctx)
{
    (void)ctx;
}

} // namespace lux::render
