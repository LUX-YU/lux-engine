/**
 * @file LineListTransientFeature.cpp
 * @brief Transient (current-frame-only) line-list gizmo rendering feature.
 */

#include <array>
#include <lux/engine/function/render/features/gizmo/GizmoVertex.hpp>
#include <lux/engine/render/renderer/features/gizmo/LineListTransientFeature.hpp>

#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/renderer/features/TransientPrimitivePipelinePreset.hpp>

#include <cstring>
#include <vk_mem_alloc.h>

namespace lux::render
{
    namespace
    {
        /// Line-list gizmo preset — GizmoVertex layout. Feature-local (not in
        /// pipeline/PipelinePresets.hpp): hard-codes a gizmo-domain vertex type, so
        /// the generic pipeline layer stays domain-free.
        GraphicsPipelineTemplate makeLineListGizmoTemplate()
        {
            const std::array attributes{
                VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GizmoVertex, x)},
                VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32_UINT, offsetof(GizmoVertex, packed_attr)},
            };
            return detail::makeTransientPrimitivePipelineTemplate(
                sizeof(GizmoVertex),
                attributes,
                VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                EGeometryType::LINE_SEGMENTS,
                false
            );
        }
    } // namespace

    LineListTransientFeature::LineListTransientFeature(Config cfg)
        : RenderFeature(RenderFeature::Config{.name = "LineListTransient"}), cfg_(std::move(cfg))
    {
    }

    // An attached ring owns its buffers; runtime detach transfers them to retirement.
    LineListTransientFeature::~LineListTransientFeature() = default;

    // ============================================================================
    //  Initialisation
    // ============================================================================

    lux::render::Expected<void> LineListTransientFeature::initAndAttachTo(RenderScene& /*scene*/)
    {
        // Self-contained feature: only the narrow RenderContextView / RenderSceneView
        // SDK surface — no engine-internal RenderContext / RenderScene / ShaderResources.
        auto cv = contextView();

        // ---- Shaders ----
        // 解析内置默认 + 域合并切换 + 取模块反射,一次做完。引用引擎契约资源(本 vert
        // 用 uViews)的管线必须带域合并标记,否则注册被拒。
        const std::array stage_requests{
            RenderContextView::PipelineStageDesc{EBuiltinShader::LINE_LIST_VERT, cfg_.vertex_shader},
            RenderContextView::PipelineStageDesc{EBuiltinShader::LINE_LIST_FRAG, cfg_.fragment_shader}
        };

        auto stages = cv.preparePipelineStages(stage_requests);
        if (!stages)
        {
            return lux::cxx::unexpected(stages.error());
        }
        auto tmpl = makeLineListGizmoTemplate();
        tmpl.descriptor_set_count = 1;
        tmpl.line_width = cfg_.line_width;
        tmpl.vertex_shader = stages->module(0);
        tmpl.fragment_shader = stages->module(1);
        // The layout is left empty -> built from reflection (this pipeline
        // only uses the Scene set, and the contract routes it back to the
        // same engine-shared layout, equivalent to building it by hand).
        tmpl.debug_name = "LineListTransient";
        pipeline_handle_ = cv.registerGraphics(tmpl, stages->infos());

        // ---- GPU ring buffers (HOST_VISIBLE, persistently mapped; shared FIF ring) ----
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
        incoming_ = &sceneView().resources().ensure<TransientLineListBuffer>();
        return {};
    }

    // ============================================================================
    //  Frame lifecycle
    // ============================================================================

    void LineListTransientFeature::onFrameBegin(const FeatureFrameContext& ctx)
    {
        if (!ring_)
        {
            draw_count_ = 0;
            return;
        }
        // The engine's REAL frame-in-flight picks the slot (rule encoded in the ring —
        // the old private frame counter could desync and overwrite an in-flight slot).
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

    void LineListTransientFeature::addPasses(RGBuilder& builder)
    {
        builder.addPass("ForwardLineListTransient", ERGPassType::GRAPHICS)
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
            .setKernel("LineListTransientDraw")
            .stage(ERenderStage::OVERLAY_STAGE); // overlay — composited on top of the post-processed
                                                 // (tonemapped) image, after the grid
    }

    void LineListTransientFeature::onDetachFromScene(RenderScene& /*scene*/)
    {
        // Runtime removeFeature has no GPU idle wait; frames N-1/N-2 may still bind
        // these ring buffers. Retire through the FIF deferred-destroy queue rather
        // than the inline destroy the destructor would do; retireInto nulls the
        // handles so the destructor is a no-op. (#17)
        auto cv = contextView();
        if (!ring_)
        {
            return;
        }
        std::move(*ring_).retireInto([&](VkBuffer b, VmaAllocation a) noexcept { cv.retireBuffer(b, a); });
        ring_.reset();
        draw_count_ = 0;
    }

} // namespace lux::render
