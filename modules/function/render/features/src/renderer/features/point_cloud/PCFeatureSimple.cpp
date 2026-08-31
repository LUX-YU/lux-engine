/**
 * @file PCFeatureSimple.cpp
 * @brief Simple point cloud rendering feature (FramePack patch upload).
 */

#include <array>
#include <lux/engine/render/renderer/features/point_cloud/PCFeatureSimple.hpp>
#include <lux/engine/render/resources/point_cloud/PointCloudGpuData.hpp>
#include <lux/engine/render/resources/point_cloud/PointCloudGlobalBuffer.hpp>

#include <lux/engine/render/resources/PointCloudResources.hpp>
#include <lux/engine/render/gpu/transfer/TransferContributor.hpp> // makeTransferContributor
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PointCloudPipelinePreset.hpp>
// makePointCloudTemplate (+ GraphicsPipelineTemplate, vulkan.h)

namespace lux::render
{

    PCFeatureSimple::PCFeatureSimple(Config cfg)
        : IPointCloudFeature(RenderFeature::Config{.name = "PointCloudSimple"}), point_size_(cfg.initial_point_size),
          cfg_(std::move(cfg))
    {
    }

    lux::render::Expected<void> PCFeatureSimple::initAndAttachTo(RenderScene& /*scene*/)
    {
        // Self-contained feature: only the narrow RenderContextView / RenderSceneView
        // SDK surface — no engine-internal RenderContext / RenderScene / ShaderResources.
        auto cv = contextView();
        // 解析内置默认 + 域合并切换 + 取模块反射,一次做完。引用引擎契约资源(本 vert
        // 用 uViews)的管线必须带域合并标记,否则注册被拒。
        const std::array stage_requests{
            RenderContextView::PipelineStageDesc{EBuiltinShader::PC_SIMPLE_VERT, cfg_.vertex_shader},
            RenderContextView::PipelineStageDesc{EBuiltinShader::PC_SIMPLE_FRAG, cfg_.fragment_shader}};

        auto stages = cv.preparePipelineStages(stage_requests);
        if (!stages)
            return lux::cxx::unexpected(stages.error());
        const auto& cfg = cfg_;

        // ---- Pipeline ----
        auto tmpl = makePointCloudTemplate();
        tmpl.descriptor_set_count = 1;
        tmpl.vertex_shader = stages->module(0);
        tmpl.fragment_shader = stages->module(1);
        // Layout left empty here; it's built via reflection instead — this
        // pipeline only uses the Scene set, and the contract routes it back to
        // the same shared engine layout, equivalent to building it by hand.
        tmpl.debug_name = "PointCloudSimple";
        pipeline_handle_ = cv.registerGraphics(tmpl, stages->infos());

        // ---- Shared GPU resources (per-scene, lazy registration) ----
        auto sv = sceneView();
        // ensure<T>(init_args):构造 + init + 只在成功时发布。分配失败时注册表里
        // 什么都没有,不会像以前那样留下一个"已发布但空"的资源让 attach 照常成功、
        // 到绘制期才以「按未初始化缓冲画」的形式暴露。
        // ⚠️ 这个类型被两个点云 feature 用**不同容量** ensure —— 命中路径丢弃实参是
        //    既有行为(谁先到谁定容量),这里保持不变。
        auto pc_r = sv.resources().ensure<PointCloudResources>(
            cv.vmaAllocator(),
            cfg.max_global_points,
            cfg.max_octree_nodes
        );
        if (!pc_r)
            return lux::cxx::unexpected<RenderError>(pc_r.error());
        auto* pc_res = *pc_r;
        pc_res->setDeferredQueue(&cv.deferredDestroyQueue());
        pc_res->setRetireScheduler(&cv.retireScheduler());
        pc_res->setRetireOwnerToken(sv.retireOwnerToken());
        // Register PointCloudResources as a transfer contributor once (idempotent:
        // shared across PC features; only the first attach adds it). The scene core
        // used to do this lazily in recordUploads — the OWNER does it now.
        if (!pc_res->usesTransferScheduler())
        {
            sv.transferScheduler().contributors().add(makeTransferContributor(pc_res, /*priority=*/1));
            pc_res->setUseTransferScheduler(true);
        }
        global_buf_ = &pc_res->globalBuffer();
        return {};
    }

    // ============================================================================
    //  RenderFeature
    // ============================================================================

    void PCFeatureSimple::addPasses(RGBuilder& builder)
    {
        builder.addPass("ForwardPointCloudSimple", ERGPassType::GRAPHICS)
            .write(builder.referenceTexture(cfg_.color_target), lux::render::ETextureRole::COLOR_ATTACHMENT)
            .write(builder.referenceTexture(cfg_.depth_target), lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
            .setPipeline(pipeline_handle_)
            .bindSceneDS()
            .setPhaseMask(phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::PointCloud)))
            .setKernelFn([this](const PassRecordContext& ctx) {
                if (!global_buf_ || global_buf_->buffer() == VK_NULL_HANDLE)
                    return;
                if (ctx.view == nullptr)
                    return;

                // Push point_size constant to the vertex shader.
                const float point_size = point_size_;
                vkCmdPushConstants(
                    ctx.cmd,
                    ctx.pipeline_layout,
                    ctx.pc_stage_flags,
                    kViewPushPrefixSize,
                    sizeof(float),
                    &point_size
                );

                // Bind VB once at offset 0; use firstVertex to select per-slot data.
                VkBuffer vbuf = global_buf_->buffer();
                VkDeviceSize zero_offset = 0;
                vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vbuf, &zero_offset);

                global_buf_->forEachSlot([&](uint32_t /*chunk_id*/, const PointCloudGlobalBuffer::Slot& slot) {
                    if (slot.count == 0)
                        return;
                    vkCmdDraw(ctx.cmd, slot.count, 1, slot.first, 0);
                }
                );
            }
            )
            .setKernel("PointCloudDraw");
    }

    void PCFeatureSimple::onFrameBegin(const FeatureFrameContext& ctx)
    {
        // Nothing to do frame-to-frame: the transfer agent drains pending patches.
        (void)ctx;
    }

} // namespace lux::render
