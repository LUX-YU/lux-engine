/**
 * @file TriOverlayTransientFeature.cpp
 * @brief Transient (current-frame-only) triangle-overlay gizmo rendering feature.
 */

#include <array>
#include <lux/engine/render/renderer/features/gizmo/TriOverlayTransientFeature.hpp>
#include <lux/engine/function/render/client/features/gizmo/GizmoVertex.hpp>

#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <lux/engine/render/renderer/features/TransientPrimitivePipelinePreset.hpp>

#include <vk_mem_alloc.h>
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

    TriOverlayTransientFeature::~TriOverlayTransientFeature()
    {
        destroySlotBuffers();
    }

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
            RenderContextView::PipelineStageDesc{EBuiltinShader::TRI_OVERLAY_FRAG, cfg_.fragment_shader}};

        auto stages = cv.preparePipelineStages(stage_requests);
        if (!stages)
            return lux::cxx::unexpected(stages.error());

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
        allocator_ = cv.vmaAllocator();
        createSlotBuffers();

        // ---- Scene-registry bridge for the upload handler ----
        incoming_ = &sceneView().resources().ensure<TransientTriOverlayBuffer>();
        return {};
    }

    // ============================================================================
    //  Frame lifecycle
    // ============================================================================

    void TriOverlayTransientFeature::onFrameBegin(const FeatureFrameContext& /*ctx*/)
    {
        active_slot_ = frame_counter_ % kBufferCount;
        ++frame_counter_;

        auto data = incoming_->take();
        if (data.empty())
        {
            draw_count_ = 0;
            return;
        }

        const uint32_t count = static_cast<uint32_t>(std::min<size_t>(data.size(), cfg_.max_vertices));

        auto& slot = slots_[active_slot_];
        assert(slot.mapped && "FrameSlot buffer was not created");

        std::memcpy(slot.mapped, data.data(), count * sizeof(GizmoVertex));
        vmaFlushAllocation(allocator_, slot.alloc, 0, count * sizeof(GizmoVertex));
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
            .setPhaseMask(phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::Gizmo)))
            .setKernelFn([this](const PassRecordContext& ctx) {
                if (draw_count_ == 0)
                    return;
                if (ctx.view == nullptr)
                    return;

                auto& slot = slots_[active_slot_];

                VkDeviceSize zero_offset = 0;
                vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &slot.buffer, &zero_offset);
                vkCmdDraw(ctx.cmd, draw_count_, 1, 0, 0);
            }
            )
            .setKernel("TriOverlayTransientDraw")
            .stage(ERenderStage::Overlay); // overlay — composited on top of the post-processed
                                           // (tonemapped) image, like the grid/line gizmos
    }

    // ============================================================================
    //  Buffer management
    // ============================================================================

    void TriOverlayTransientFeature::createSlotBuffers()
    {
        const VkDeviceSize byte_size = static_cast<VkDeviceSize>(cfg_.max_vertices) * sizeof(GizmoVertex);

        for (auto& slot : slots_)
        {
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = byte_size;
            bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo info{};
            VkResult r = vmaCreateBuffer(allocator_, &bci, &aci, &slot.buffer, &slot.alloc, &info);
            if (r != VK_SUCCESS)
                renderFatal("TriOverlayTransientFeature buffer creation failed");
            slot.mapped = info.pMappedData;
        }
    }

    void TriOverlayTransientFeature::onDetachFromScene(RenderScene& /*scene*/)
    {
        // Runtime removeFeature has no GPU idle wait; frames N-1/N-2 may still bind
        // these ring buffers. Retire through the FIF deferred-destroy queue rather
        // than the inline destroy the destructor would do. Nulled handles make
        // destroySlotBuffers() a no-op on this path. (#17)
        auto cv = contextView();
        for (auto& slot : slots_)
        {
            if (slot.buffer != VK_NULL_HANDLE)
                cv.retireBuffer(slot.buffer, slot.alloc);
            slot = {};
        }
    }

    void TriOverlayTransientFeature::destroySlotBuffers()
    {
        // Fallback for the no-detach path (device idle at shutdown); a no-op after
        // onDetachFromScene() has retired + nulled the handles.
        if (!allocator_)
            return;
        for (auto& slot : slots_)
        {
            if (slot.buffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator_, slot.buffer, slot.alloc);
            slot = {};
        }
    }

} // namespace lux::render
