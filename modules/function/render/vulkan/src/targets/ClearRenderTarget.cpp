#include <lux/engine/render/targets/RenderTargetBinding.hpp>

namespace lux::render
{
bool clearRenderTarget(VkCommandBuffer commands, const RenderTargetBinding &target, std::uint32_t frame_slot,
                       std::uint32_t image_index) noexcept
{
    if (!commands || !target.layout || !target.extent.width || !target.extent.height)
    {
        return false;
    }
    bool recorded{};
    for (std::size_t index{}; index < kTargetSlotCount; ++index)
    {
        if (!target.layout->slots[index])
        {
            continue;
        }
        const auto &description = *target.layout->slots[index];
        const auto &images = target.slot_images[index];
        // makeFrameBinding may already select the acquired/FIF image into a
        // singleton. Match graph recording's binding contract, not swapchain size.
        const auto selected = frame_slot < images.images.size() ? frame_slot : 0;
        if (selected >= images.images.size() || selected >= images.views.size())
        {
            continue;
        }
        const bool color = description.aspect == ERenderAspect::COLOR;
        const auto aspect = toVkImageAspect(description.aspect);
        const auto attachment =
            color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const auto sync = finalSyncForState(color ? ERenderResourceState::COLOR_ATTACHMENT
                                                  : ERenderResourceState::DEPTH_STENCIL_ATTACHMENT);
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = sync.stage;
        barrier.dstAccessMask = sync.access;
        // First layer discards previous contents, but still orders earlier GPU
        // uses. A later layer preserves the attachment handed over by its predecessor.
        barrier.oldLayout = description.preserve_content ? attachment : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = attachment;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images.images[selected];
        barrier.subresourceRange = {aspect, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dependency);

        VkRenderingAttachmentInfo output{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        output.imageView = images.views[selected];
        output.imageLayout = attachment;
        output.loadOp = description.preserve_content ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        output.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (color)
        {
            // Integer auxiliary outputs use zero (no object). SceneColor is opaque black.
            output.clearValue.color = {{0, 0, 0, index == std::size_t(TargetSlot::SCENE_COLOR) ? 1.0f : 0.0f}};
        }
        else
        {
            output.clearValue.depthStencil = {1.0f, 0};
        }
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = target.extent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = color ? 1 : 0;
        rendering.pColorAttachments = color ? &output : nullptr;
        rendering.pDepthAttachment = (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? &output : nullptr;
        rendering.pStencilAttachment = (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? &output : nullptr;
        vkCmdBeginRendering(commands, &rendering);
        vkCmdEndRendering(commands);

        const auto final = finalSyncForState(description.final_state);
        barrier.srcStageMask = sync.stage;
        barrier.srcAccessMask = sync.access;
        barrier.dstStageMask = final.stage;
        barrier.dstAccessMask = final.access;
        barrier.oldLayout = attachment;
        barrier.newLayout = toVkImageLayout(description.final_state);
        vkCmdPipelineBarrier2(commands, &dependency);
        recorded = true;
    }
    return recorded;
}
} // namespace lux::render
