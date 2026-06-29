/**
 * @file LineListTransientFeature.cpp
 * @brief Transient (current-frame-only) line-list gizmo rendering feature.
 */

#include <lux/engine/render/renderer/features/gizmo/LineListTransientFeature.hpp>
#include <lux/engine/render/renderer/features/gizmo/GizmoVertex.hpp>

#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>  // GraphicsPipelineTemplate (+ vulkan.h)

#include <vk_mem_alloc.h>
#include <cassert>
#include <cstring>

namespace lux::render
{
    namespace
    {
        /// Line-list gizmo preset — GizmoVertex layout. Feature-local (not in
        /// pipeline/PipelinePresets.hpp): hard-codes a gizmo-domain vertex type, so
        /// the generic pipeline layer stays domain-free.
        GraphicsPipelineTemplate makeLineListGizmoTemplate()
        {
            GraphicsPipelineTemplate tmpl{};
            tmpl.geometry_type = EGeometryType::LINE_SEGMENTS;

            VkVertexInputBindingDescription b{};
            b.binding   = 0;
            b.stride    = static_cast<uint32_t>(sizeof(GizmoVertex));
            b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            tmpl.vertex_bindings = { b };

            tmpl.vertex_attributes = {
                VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GizmoVertex, x)},
                VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32_UINT,          offsetof(GizmoVertex, packed_attr)},
            };

            tmpl.topology           = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            tmpl.polygon_mode       = VK_POLYGON_MODE_FILL;
            tmpl.cull_mode          = VK_CULL_MODE_NONE;
            tmpl.front_face         = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            tmpl.depth_test_enable   = VK_TRUE;
            tmpl.depth_write_enable  = VK_FALSE;
            tmpl.depth_compare_op    = VK_COMPARE_OP_LESS_OR_EQUAL;
            tmpl.blend_enable        = VK_FALSE;
            tmpl.src_color_blend_factor = VK_BLEND_FACTOR_ONE;
            tmpl.dst_color_blend_factor = VK_BLEND_FACTOR_ZERO;
            tmpl.color_blend_op      = VK_BLEND_OP_ADD;
            tmpl.src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
            tmpl.dst_alpha_blend_factor = VK_BLEND_FACTOR_ZERO;
            tmpl.alpha_blend_op      = VK_BLEND_OP_ADD;
            tmpl.color_write_mask    = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            tmpl.use_dynamic_viewport = true;
            tmpl.use_dynamic_scissor  = true;
            return tmpl;
        }
    } // namespace

    LineListTransientFeature::LineListTransientFeature(Config cfg)
        : RenderFeature(RenderFeature::Config{.name = "LineListTransient"}), cfg_(std::move(cfg))
    {
    }

    LineListTransientFeature::~LineListTransientFeature()
    {
        destroySlotBuffers();
    }

    // ============================================================================
    //  Initialisation
    // ============================================================================

    void LineListTransientFeature::initAndAttachTo(RenderScene & /*scene*/)
    {
        // Self-contained feature: only the narrow RenderContextView / RenderSceneView
        // SDK surface — no engine-internal RenderContext / RenderScene / ShaderResources.
        auto cv = contextView();

        // ---- Shaders ----
        cfg_.vertex_shader   = cv.loadBuiltinShader(EBuiltinShader::LINE_LIST_VERT, cfg_.vertex_shader);
        cfg_.fragment_shader = cv.loadBuiltinShader(EBuiltinShader::LINE_LIST_FRAG, cfg_.fragment_shader);

        // ---- Pipeline ----
        auto tmpl = makeLineListGizmoTemplate();
        tmpl.descriptor_set_count = 1;
        tmpl.line_width = cfg_.line_width;
        tmpl.vertex_shader   = cv.shaderModule(cfg_.vertex_shader);
        tmpl.fragment_shader = cv.shaderModule(cfg_.fragment_shader);

        std::vector<const lux::rdesc::ShaderInfo *> infos = {
            cv.shaderInfo(cfg_.vertex_shader),
            cv.shaderInfo(cfg_.fragment_shader)
        };
        tmpl.pipeline_layout = cv.buildStandardGraphicsLayout(
            tmpl.descriptor_set_count, infos, "LineListTransientLayout");
        pipeline_handle_ = cv.registerGraphics(tmpl, infos);

        // ---- GPU ring buffers (HOST_VISIBLE, persistently mapped) ----
        allocator_ = cv.vmaAllocator();
        createSlotBuffers();

        // ---- Scene-registry bridge for the upload handler ----
        incoming_ = sceneView().sceneRegistry().ensure<TransientLineListBuffer>();
    }

    // ============================================================================
    //  Frame lifecycle
    // ============================================================================

    void LineListTransientFeature::onFrameBegin(const FeatureFrameContext & /*ctx*/)
    {
        active_slot_ = frame_counter_ % kBufferCount;
        ++frame_counter_;

        auto data = incoming_->take();
        if (data.empty())
        {
            draw_count_ = 0;
            return;
        }

        const uint32_t count = static_cast<uint32_t>(
            std::min<size_t>(data.size(), cfg_.max_vertices));

        auto &slot = slots_[active_slot_];
        assert(slot.mapped && "FrameSlot buffer was not created");

        std::memcpy(slot.mapped, data.data(), count * sizeof(GizmoVertex));
        vmaFlushAllocation(allocator_, slot.alloc, 0,
                           count * sizeof(GizmoVertex));
        draw_count_ = count;
    }

    // ============================================================================
    //  Render graph
    // ============================================================================

    void LineListTransientFeature::addPasses(RGBuilder &builder)
    {
        builder.addPass("ForwardLineListTransient", ERGPassType::GRAPHICS)
            .write(builder.referenceTexture(cfg_.color_target), lux::common::ETextureRole::COLOR_ATTACHMENT)
            .write(builder.referenceTexture(cfg_.depth_target), lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
            .setPipeline(pipeline_handle_)
            .bindSceneDS(0)
            .setPhaseMask(phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::Gizmo)))
            .setKernelFn([this](const PassRecordContext &ctx)
                         {
            if (draw_count_ == 0) return;
            if (ctx.view == nullptr) return;

            auto& slot = slots_[active_slot_];

            VkDeviceSize zero_offset = 0;
            vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &slot.buffer, &zero_offset);
            vkCmdDraw(ctx.cmd, draw_count_, 1, 0, 0); })
            .setKernel("LineListTransientDraw")
            .stage(ERenderStage::Overlay); // overlay — composited on top of the post-processed
                                           // (tonemapped) image, after the grid
    }

    // ============================================================================
    //  Buffer management
    // ============================================================================

    void LineListTransientFeature::createSlotBuffers()
    {
        const VkDeviceSize byte_size =
            static_cast<VkDeviceSize>(cfg_.max_vertices) * sizeof(GizmoVertex);

        for (auto &slot : slots_)
        {
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = byte_size;
            bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo info{};
            VkResult r = vmaCreateBuffer(allocator_, &bci, &aci,
                                         &slot.buffer, &slot.alloc, &info);
            assert(r == VK_SUCCESS && "Failed to create transient line list buffer");
            (void)r;
            slot.mapped = info.pMappedData;
        }
    }

    void LineListTransientFeature::onDetachFromScene(RenderScene & /*scene*/)
    {
        // Runtime removeFeature has no GPU idle wait; frames N-1/N-2 may still bind
        // these ring buffers. Retire through the FIF deferred-destroy queue rather
        // than the inline destroy the destructor would do. Nulled handles make
        // destroySlotBuffers() a no-op on this path. (#17)
        auto cv = contextView();
        for (auto &slot : slots_)
        {
            if (slot.buffer != VK_NULL_HANDLE)
                cv.retireBuffer(slot.buffer, slot.alloc);
            slot = {};
        }
    }

    void LineListTransientFeature::destroySlotBuffers()
    {
        // Fallback for the no-detach path (device idle at shutdown); a no-op after
        // onDetachFromScene() has retired + nulled the handles.
        if (!allocator_)
            return;
        for (auto &slot : slots_)
        {
            if (slot.buffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator_, slot.buffer, slot.alloc);
            slot = {};
        }
    }

} // namespace lux::render
