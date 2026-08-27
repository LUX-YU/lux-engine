/**
 * @file PCFeatureTransient.cpp
 * @brief Transient (current-frame-only) point cloud rendering feature.
 */

#include <lux/engine/render/renderer/features/point_cloud/PCFeatureTransient.hpp>
#include <lux/engine/render/resources/point_cloud/PointCloudGpuData.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>

#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/StandardPipelineLayoutBuilder.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PointCloudPipelinePreset.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>

#include <vk_mem_alloc.h>
#include <cassert>
#include <cstring>

namespace lux::render
{

    PCFeatureTransient::PCFeatureTransient(Config cfg)
        : IPointCloudFeature(RenderFeature::Config{.name = "PointCloudTransient"}), cfg_(std::move(cfg)),
          point_size_(cfg_.point_size)
    {
    }

    // The ring destroys itself (no-detach fallback; a runtime detach retired + nulled first).
    PCFeatureTransient::~PCFeatureTransient() = default;

    // ============================================================================
    //  Initialisation
    // ============================================================================

    lux::render::Expected<void> PCFeatureTransient::initAndAttachTo(RenderScene& /*scene*/)
    {
        auto& ctx = renderContext();

        // ---- Shaders ----
        auto& shaders = ctx.globalRegistry().must<ShaderResources>();
        // 解析内置默认 + 域合并切换 + 取模块反射,一次做完。引用引擎契约资源(本 vert
        // 用 uViews)的管线必须带域合并标记,否则 PipelineManager 拒绝注册。
        const std::array stage_requests{
            PipelineStageRequest{EBuiltinShader::PC_SIMPLE_VERT, cfg_.vertex_shader},
            PipelineStageRequest{EBuiltinShader::PC_SIMPLE_FRAG, cfg_.fragment_shader}};

        auto stages = preparePipelineStages(shaders, stage_requests);
        if (!stages)
            return lux::cxx::unexpected(stages.error());

        // ---- Pipeline ----
        auto tmpl = makePointCloudTemplate();
        tmpl.descriptor_set_count = 1;
        tmpl.vertex_shader = stages->module(0);
        tmpl.fragment_shader = stages->module(1);

        // Layout left empty here; it's built via reflection instead — this
        // pipeline only uses the Scene set, and the contract routes it back to
        // the same shared engine layout, equivalent to building it by hand.
        tmpl.debug_name = "PointCloudTransient";
        auto pipeline = ctx.pipelineManager().registerGraphicsTemplate(tmpl, stages->infos());
        if (!pipeline)
            return lux::cxx::unexpected(pipeline.error());
        pipeline_handle_ = *pipeline;

        // ---- GPU ring buffers (HOST_VISIBLE, persistently mapped; shared FIF ring) ----
        if (auto r = ring_.create(
                ctx.vmaAllocator(),
                contextView().framesInFlight(),
                static_cast<VkDeviceSize>(cfg_.max_points) * sizeof(GpuPointVertex));
            !r)
            return r;

        // ---- Scene-registry bridge for the upload handler ----
        if (!renderScene().sceneRegistry().find<TransientPointCloudBuffer>())
            renderScene().sceneRegistry().emplace<TransientPointCloudBuffer>();
        incoming_ = renderScene().sceneRegistry().find<TransientPointCloudBuffer>();
        return {};
    }

    // ============================================================================
    //  Frame lifecycle
    // ============================================================================

    void PCFeatureTransient::onFrameBegin(const FeatureFrameContext& ctx)
    {
        if (ring_.empty())
        {
            draw_count_ = 0;
            return;
        }
        // The engine's REAL frame-in-flight picks the slot (rule encoded in the ring —
        // the old private frame counter could desync and overwrite an in-flight slot).
        active_slot_ = ring_.slotIndexFor(ctx.frame_index);

        auto data = incoming_->take();
        if (data.empty())
        {
            draw_count_ = 0;
            return;
        }

        const uint32_t count = static_cast<uint32_t>(std::min<size_t>(data.size(), cfg_.max_points));

        auto& slot = ring_.slotAt(active_slot_);
        assert(slot.mapped && "ring slot buffer was not created");

        std::memcpy(slot.mapped, data.data(), count * sizeof(GpuPointVertex));
        ring_.flush(active_slot_, count * sizeof(GpuPointVertex));
        draw_count_ = count;
    }

    // ============================================================================
    //  Render graph
    // ============================================================================

    void PCFeatureTransient::addPasses(RGBuilder& builder)
    {
        builder.addPass("ForwardPointCloudTransient", ERGPassType::GRAPHICS)
            .write(builder.referenceTexture(cfg_.color_target), lux::render::ETextureRole::COLOR_ATTACHMENT)
            .write(builder.referenceTexture(cfg_.depth_target), lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
            .setPipeline(pipeline_handle_)
            .bindSceneDS()
            .setPhaseMask(phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::PointCloud)))
            .setKernelFn([this](const PassRecordContext& ctx) {
                if (draw_count_ == 0)
                    return;
                if (ctx.view == nullptr)
                    return;

                auto& slot = ring_.slotAt(active_slot_);

                const float point_size = point_size_;
                vkCmdPushConstants(
                    ctx.cmd,
                    ctx.pipeline_layout,
                    ctx.pc_stage_flags,
                    kViewPushPrefixSize,
                    sizeof(float),
                    &point_size
                );

                VkDeviceSize zero_offset = 0;
                vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &slot.buffer, &zero_offset);
                vkCmdDraw(ctx.cmd, draw_count_, 1, 0, 0);
            }
            )
            .setKernel("PointCloudTransientDraw");
    }

    // ============================================================================
    //  Buffer management
    // ============================================================================

    void PCFeatureTransient::onDetachFromScene(RenderScene& /*scene*/)
    {
        // Runtime removeFeature has NO GPU idle wait, and these HOST_VISIBLE ring
        // buffers are bound by frames N-1/N-2 still executing on the GPU. Retire them
        // through the frames-in-flight deferred-destroy queue instead of destroying
        // inline; retireInto nulls the handles so the destructor is a no-op. (#17)
        auto& q = renderContext().deferredDestroyQueue();
        ring_.retireInto([&](VkBuffer b, VmaAllocation a) { q.retireBuffer(b, a); });
    }

} // namespace lux::render
