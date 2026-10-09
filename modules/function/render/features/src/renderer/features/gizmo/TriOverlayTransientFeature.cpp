/**
 * @file TriOverlayTransientFeature.cpp
 * @brief Transient (current-frame-only) triangle-overlay gizmo rendering feature.
 */

#include <array>
#include <lux/engine/function/render/features/gizmo/GizmoVertex.hpp>
#include <lux/engine/render/renderer/features/gizmo/TriOverlayTransientFeature.hpp>

#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/renderer/features/TransientPrimitivePipelinePreset.hpp>

#include <cstring>

namespace lux::render
{

    namespace
    {
        /// Triangle-overlay gizmo preset (alpha-blended) — GizmoVertex layout.
        /// Feature-local (not in pipeline/PipelinePresets.hpp): hard-codes a gizmo-
        /// domain vertex type, so the generic pipeline layer stays domain-free.
        GraphicsPipelineTemplate makeTriOverlayGizmoTemplate()
        {
            const std::array attributes{
                VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GizmoVertex, x)},
                VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32_UINT, offsetof(GizmoVertex, packed_attr)},
            };
            return detail::makeTransientPrimitivePipelineTemplate(
                sizeof(GizmoVertex),
                attributes,
                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                EGeometryType::MESH,
                true
            );
        }
    } // namespace

    TriOverlayTransientFeature::TriOverlayTransientFeature(Config cfg)
        : RenderFeature(RenderFeature::Config{.name = "TriOverlayTransient"}), cfg_(std::move(cfg))
    {
    }

    TriOverlayTransientFeature::~TriOverlayTransientFeature() = default;

    // ============================================================================
    //  Initialisation
    // ============================================================================

    lux::render::Expected<void> TriOverlayTransientFeature::initAndAttachTo(RenderScene& /*scene*/)
    {
        // Self-contained feature: only the narrow RenderContextView / RenderSceneView
        // SDK surface — no engine-internal RenderContext / RenderScene / ShaderResources.
        auto cv = contextView();

        // ---- Shaders ----
        // 解析内置默认 + 域合并切换 + 取模块反射,一次做完。引用引擎契约资源(本 vert
        // 用 uViews)的管线必须带域合并标记,否则注册被拒。
        const std::array stage_requests{
            RenderContextView::PipelineStageDesc{EBuiltinShader::TRI_OVERLAY_VERT, cfg_.vertex_shader},
            RenderContextView::PipelineStageDesc{EBuiltinShader::TRI_OVERLAY_FRAG, cfg_.fragment_shader}
        };

        auto stages = cv.preparePipelineStages(stage_requests);
        if (!stages)
        {
            return lux::cxx::unexpected(stages.error());
        }

        // ---- Pipeline ----
        auto tmpl = makeTriOverlayGizmoTemplate();
        tmpl.descriptor_set_count = 1;
        tmpl.vertex_shader = stages->module(0);
        tmpl.fragment_shader = stages->module(1);
        // The layout is left empty -> built from reflection (this pipeline only
        // uses the Scene set, and the contract routes it back to the same
        // engine-shared layout, equivalent to building it by hand).
        tmpl.debug_name = "TriOverlayTransient";
        pipeline_handle_ = cv.registerGraphics(tmpl, stages->infos());

        // ---- GPU ring buffers (HOST_VISIBLE, persistently mapped) ----
        auto ring = TransientVertexRing::create(
            cv.vmaAllocator(),
            cv.framesInFlight(),
            static_cast<VkDeviceSize>(cfg_.max_vertices) * sizeof(GizmoVertex)
        );
        if (!ring)
        {
            return lux::cxx::unexpected(ring.error());
        }
        ring_.emplace(std::move(*ring));

        // ---- Scene-registry bridge for the upload handler ----
        incoming_ = &sceneView().resources().ensure<TransientTriOverlayBuffer>();
        return {};
    }

    // ============================================================================
    //  Frame lifecycle
    // ============================================================================

    void TriOverlayTransientFeature::onFrameBegin(const FeatureFrameContext& ctx)
    {
        if (!ring_)
        {
            draw_count_ = 0;
            return;
        }
        active_slot_ = ring_->slotIndexFor(ctx.frame_index);

        auto data = incoming_->take();
        if (data.empty())
        {
            draw_count_ = 0;
            return;
        }

        const uint32_t count = static_cast<uint32_t>(std::min<size_t>(data.size(), cfg_.max_vertices));

        auto& slot = ring_->slotAt(active_slot_);

        std::memcpy(slot.mapped, data.data(), count * sizeof(GizmoVertex));
        ring_->flush(active_slot_, count * sizeof(GizmoVertex));
        draw_count_ = count;
    }

    // ============================================================================
    //  Render graph
    // ============================================================================

    void TriOverlayTransientFeature::addPasses(RGBuilder& builder)
    {
        builder.addPass("ForwardTriOverlayTransient", ERGPassType::GRAPHICS)
            .write(builder.referenceTexture(cfg_.color_target), lux::render::ETextureRole::COLOR_ATTACHMENT)
            .write(builder.referenceTexture(cfg_.depth_target), lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
            .setPipeline(pipeline_handle_)
            .bindSceneDS()
            .setPhaseMask(phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::GIZMO)))
            .setKernelFn(
                [this](const PassRecordContext& ctx)
                {
                    if (draw_count_ == 0)
                    {
                        return;
                    }
                    if (ctx.view == nullptr)
                    {
                        return;
                    }

                    auto& slot = ring_->slotAt(active_slot_);

                    const auto buffer = slot.buffer.buffer();
                    VkDeviceSize zero_offset = 0;
                    vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &buffer, &zero_offset);
                    vkCmdDraw(ctx.cmd, draw_count_, 1, 0, 0);
                }
            )
            .setKernel("TriOverlayTransientDraw")
            .stage(ERenderStage::OVERLAY_STAGE); // overlay — composited on top of the post-processed
                                                 // (tonemapped) image, like the grid/line gizmos
    }

    void TriOverlayTransientFeature::onDetachFromScene(RenderScene& /*scene*/)
    {
        if (!ring_)
        {
            return;
        }
        // Frames in flight retain the buffers through the original retirement sink.
        auto cv = contextView();
        std::move(*ring_).retireInto([&](VkBuffer buffer, VmaAllocation allocation) noexcept
                                     { cv.retireBuffer(buffer, allocation); });
        ring_.reset();
        draw_count_ = 0;
    }
} // namespace lux::render
