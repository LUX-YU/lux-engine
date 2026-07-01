/**
 * @file PCFeatureTransient.cpp
 * @brief Transient (current-frame-only) point cloud rendering feature.
 */

#include <lux/engine/render/renderer/features/point_cloud/PCFeatureTransient.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PointCloudGpuData.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>

#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/StandardPipelineLayoutBuilder.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PointCloudPipelinePreset.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>

#include <vk_mem_alloc.h>
#include <cassert>
#include <cstring>

namespace lux::render
{

PCFeatureTransient::PCFeatureTransient(Config cfg)
    : IPointCloudFeature(RenderFeature::Config{.name = "PointCloudTransient"})
    , cfg_(std::move(cfg))
    , point_size_(cfg_.point_size)
{
}

PCFeatureTransient::~PCFeatureTransient()
{
    destroySlotBuffers();
}

// ============================================================================
//  Initialisation
// ============================================================================

lux::render::Expected<void> PCFeatureTransient::initAndAttachTo(RenderScene& /*scene*/){
    auto& ctx = renderContext();

    // ---- Shaders ----
    auto* shaders = ctx.globalRegistry().find<ShaderResources>();
    cfg_.vertex_shader   = ensureBuiltinShader(shaders, cfg_.vertex_shader,   EBuiltinShader::PC_SIMPLE_VERT);
    cfg_.fragment_shader = ensureBuiltinShader(shaders, cfg_.fragment_shader, EBuiltinShader::PC_SIMPLE_FRAG);

    const auto& vs = *shaders->get(cfg_.vertex_shader);
    const auto& fs = *shaders->get(cfg_.fragment_shader);

    // ---- Pipeline ----
    auto tmpl = makePointCloudTemplate();
    tmpl.descriptor_set_count = 1;
    tmpl.vertex_shader   = vs.module;
    tmpl.fragment_shader = fs.module;

    std::vector<const lux::rdesc::ShaderInfo*> infos = {&vs.info, &fs.info};
    tmpl.pipeline_layout = buildStandardGraphicsPipelineLayout(
        ctx, tmpl.descriptor_set_count, infos,
        "PointCloudTransientLayout").value();
    pipeline_handle_ = ctx.pipelineManager().registerGraphicsTemplate(tmpl, infos).value();

    // ---- GPU ring buffers (HOST_VISIBLE, persistently mapped) ----
    allocator_ = ctx.vmaAllocator();
    createSlotBuffers();

    // ---- Scene-registry bridge for the upload handler ----
    if (!renderScene().sceneRegistry().find<TransientPointCloudBuffer>())
        renderScene().sceneRegistry().emplace<TransientPointCloudBuffer>();
    incoming_ = renderScene().sceneRegistry().find<TransientPointCloudBuffer>();
    return {};
    }

// ============================================================================
//  Frame lifecycle
// ============================================================================

void PCFeatureTransient::onFrameBegin(const FeatureFrameContext& /*ctx*/)
{
    active_slot_ = frame_counter_ % kBufferCount;
    ++frame_counter_;

    auto data = incoming_->take();
    if (data.empty()) {
        draw_count_ = 0;
        return;
    }

    const uint32_t count = static_cast<uint32_t>(
        std::min<size_t>(data.size(), cfg_.max_points));

    auto& slot = slots_[active_slot_];
    assert(slot.mapped && "FrameSlot buffer was not created");

    std::memcpy(slot.mapped, data.data(), count * sizeof(GpuPointVertex));
    vmaFlushAllocation(allocator_, slot.alloc, 0,
                       count * sizeof(GpuPointVertex));
    draw_count_ = count;
}

// ============================================================================
//  Render graph
// ============================================================================

void PCFeatureTransient::addPasses(RGBuilder& builder)
{
    builder.addPass("ForwardPointCloudTransient", ERGPassType::GRAPHICS)
        .write(builder.referenceTexture(cfg_.color_target), lux::common::ETextureRole::COLOR_ATTACHMENT)
        .write(builder.referenceTexture(cfg_.depth_target), lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
        .setPipeline(pipeline_handle_)
        .bindSceneDS(0)
        .setPhaseMask(phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::PointCloud)))
        .setKernelFn([this](const PassRecordContext& ctx)
        {
            if (draw_count_ == 0) return;
            if (ctx.view == nullptr) return;

            auto& slot = slots_[active_slot_];

            const float point_size = point_size_.load(std::memory_order_relaxed);
            vkCmdPushConstants(ctx.cmd, ctx.pipeline_layout,
                               ctx.pc_stage_flags, 8,
                               sizeof(float), &point_size);

            VkDeviceSize zero_offset = 0;
            vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &slot.buffer, &zero_offset);
            vkCmdDraw(ctx.cmd, draw_count_, 1, 0, 0);
        })
        .setKernel("PointCloudTransientDraw");
}

// ============================================================================
//  Buffer management
// ============================================================================

void PCFeatureTransient::createSlotBuffers()
{
    const VkDeviceSize byte_size =
        static_cast<VkDeviceSize>(cfg_.max_points) * sizeof(GpuPointVertex);

    for (auto& slot : slots_) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size  = byte_size;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                  | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo info{};
        VkResult r = vmaCreateBuffer(allocator_, &bci, &aci,
                                     &slot.buffer, &slot.alloc, &info);
        assert(r == VK_SUCCESS && "Failed to create transient PC buffer");
        (void)r;
        slot.mapped = info.pMappedData;
    }
}

void PCFeatureTransient::onDetachFromScene(RenderScene& /*scene*/)
{
    // Runtime removeFeature has NO GPU idle wait, and these HOST_VISIBLE ring
    // buffers are bound by frames N-1/N-2 still executing on the GPU. Retire them
    // through the frames-in-flight deferred-destroy queue instead of destroying
    // inline (which the destructor would otherwise do). The handles are nulled so
    // the destructor's destroySlotBuffers() is a no-op on this path. (#17)
    auto& q = renderContext().deferredDestroyQueue();
    for (auto& slot : slots_) {
        if (slot.buffer != VK_NULL_HANDLE)
            q.retireBuffer(slot.buffer, slot.alloc);
        slot = {};
    }
}

void PCFeatureTransient::destroySlotBuffers()
{
    // Fallback for the no-detach path only: onDetachFromScene() (called by both
    // removeFeature and scene teardown) normally retires + nulls these first, so
    // this is a no-op there. If it ever runs with live handles, the device is
    // idle (shutdown), so immediate destruction is safe.
    if (!allocator_) return;
    for (auto& slot : slots_) {
        if (slot.buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(allocator_, slot.buffer, slot.alloc);
        slot = {};
    }
}

} // namespace lux::render
