/**
 * @file TrajectoryLineFeature.cpp
 * @brief Simple trajectory line rendering feature (LINE_STRIP draw per slot).
 */

#include <array>
#include <lux/engine/render/renderer/features/trajectory/TrajectoryLineFeature.hpp>
#include <lux/engine/render/resources/TrajectoryGpuData.hpp>
#include <lux/engine/render/resources/TrajectoryGlobalBuffer.hpp>

#include <lux/engine/render/resources/TrajectoryResources.hpp>
#include <lux/engine/render/gpu/transfer/TransferContributor.hpp> // makeTransferContributor
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/renderer/features/TransientPrimitivePipelinePreset.hpp>

namespace lux::render
{

    namespace
    {
        /// Trajectory line preset (LINE_STRIP) — GpuTrajectoryVertex layout. Feature-
        /// local (not in pipeline/PipelinePresets.hpp): it hard-codes a trajectory-
        /// domain vertex type, so the generic pipeline layer stays domain-free.
        GraphicsPipelineTemplate makeTrajectoryLineTemplate()
        {
            const std::array attributes{
                VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuTrajectoryVertex, x)},
                VkVertexInputAttributeDescription{
                    1,
                    0,
                    VK_FORMAT_R32_UINT,
                    offsetof(GpuTrajectoryVertex, packed_color)},
                VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32_SFLOAT, offsetof(GpuTrajectoryVertex, time)},
                VkVertexInputAttributeDescription{3, 0, VK_FORMAT_R32_SFLOAT, offsetof(GpuTrajectoryVertex, width)},
            };
            return detail::makeTransientPrimitivePipelineTemplate(
                sizeof(GpuTrajectoryVertex),
                attributes,
                VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
                EGeometryType::MESH,
                true,
                3.0f
            );
        }
    } // namespace

    TrajectoryLineFeature::TrajectoryLineFeature(Config cfg)
        : ITrajectoryFeature(RenderFeature::Config{.name = "TrajectoryLine"}), cfg_(std::move(cfg))
    {
    }

    lux::render::Expected<void> TrajectoryLineFeature::initAndAttachTo(RenderScene& /*scene*/)
    {
        // Self-contained feature: only the narrow RenderContextView / RenderSceneView
        // SDK surface — no engine-internal RenderContext / RenderScene / ShaderResources.
        auto cv = contextView();
        // 解析内置默认 + 域合并切换 + 取模块反射,一次做完。引用引擎契约资源(本 vert
        // 用 uViews)的管线必须带域合并标记,否则注册被拒。
        const std::array stage_requests{
            RenderContextView::PipelineStageDesc{EBuiltinShader::TRAJECTORY_LINE_VERT, cfg_.vertex_shader},
            RenderContextView::PipelineStageDesc{EBuiltinShader::TRAJECTORY_LINE_FRAG, cfg_.fragment_shader}};

        auto stages = cv.preparePipelineStages(stage_requests);
        if (!stages)
            return lux::cxx::unexpected(stages.error());
        // ---- Pipeline ----
        auto tmpl = makeTrajectoryLineTemplate();
        tmpl.descriptor_set_count = 1;
        tmpl.vertex_shader = stages->module(0);
        tmpl.fragment_shader = stages->module(1);
        // Leaving layout empty makes it build via reflection (this pipeline only
        // uses the Scene set; contract routing sends it back to the same engine
        // shared layout, equivalent to a hand-built one).
        tmpl.debug_name = "TrajectoryLine";
        pipeline_handle_ = cv.registerGraphics(tmpl, stages->infos());

        // ---- Shared GPU resources (per-scene, lazy registration) ----
        auto sv = sceneView();
        // ensure<T>(init_args): the registry inits and publishes only on success, so
        // a failed VMA allocation leaves NOTHING discoverable instead of a
        // published-but-empty resource this feature would then attach on top of.
        auto traj_r = sv.resources().ensure<TrajectoryResources>(cv.vmaAllocator(), cfg_.max_global_vertices);
        if (!traj_r)
            return lux::cxx::unexpected<RenderError>(traj_r.error());
        auto* traj_res = *traj_r;
        traj_res->setDeferredQueue(&cv.deferredDestroyQueue());
        traj_res->setRetireScheduler(&cv.retireScheduler());
        traj_res->setRetireOwnerToken(sv.retireOwnerToken());
        // Register TrajectoryResources as a transfer contributor once (idempotent).
        // The scene core used to do this lazily in recordUploads — the OWNER does it
        // now, keeping the core scene domain-free.
        if (!traj_res->usesTransferScheduler())
        {
            sv.transferScheduler().contributors().add(makeTransferContributor(traj_res, /*priority=*/2));
            traj_res->setUseTransferScheduler(true);
        }
        global_buf_ = &traj_res->globalBuffer();
        return {};
    }

    // ============================================================================
    //  RenderFeature
    // ============================================================================

    void TrajectoryLineFeature::addPasses(RGBuilder& builder)
    {
        builder.addPass("ForwardTrajectoryLine", ERGPassType::GRAPHICS)
            .write(builder.referenceTexture(cfg_.color_target), lux::render::ETextureRole::COLOR_ATTACHMENT)
            .write(builder.referenceTexture(cfg_.depth_target), lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
            .setPipeline(pipeline_handle_)
            .bindSceneDS()
            .setPhaseMask(phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::ForwardTrans)))
            .setKernelFn([this](const PassRecordContext& ctx) {
                if (!global_buf_ || global_buf_->buffer() == VK_NULL_HANDLE)
                    return;
                if (ctx.view == nullptr)
                    return;

                // Bind the unified trajectory vertex buffer once at offset 0.
                VkBuffer vbuf = global_buf_->buffer();
                VkDeviceSize zero_offset = 0;
                vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vbuf, &zero_offset);

                global_buf_->forEachTrajectory(
                    [&](uint32_t /*trajectory_id*/, const TrajectoryGlobalBuffer::Slot& slot) {
                        if (slot.count == 0)
                            return;
                        vkCmdDraw(ctx.cmd, slot.count, 1, slot.first, 0);
                    }
                );
            }
            )
            .setKernel("TrajectoryDraw");
    }

    void TrajectoryLineFeature::onFrameBegin(const FeatureFrameContext& ctx)
    {
        (void)ctx;
    }

} // namespace lux::render
