#pragma once
#include "TextureFixture.hpp"

static std::vector<std::byte> read(DeviceSession& session, const auto& version)
{
    const auto& info = version.info();
    const auto last = info.mip_ranges[info.mip_count - 1];
    auto output = take(session.allocator().createBuffer(last.offset + last.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        EMemoryAccess::READBACK));
    auto batch = take(session.queue().begin());
    batch.retain(output);
    VkImageMemoryBarrier2 image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    image.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    image.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    image.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    image.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    image.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image.image = version.image().native();
    image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, info.mip_count, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &image;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    for (unsigned mip = 0; mip < info.mip_count; ++mip)
    {
        VkBufferImageCopy region{};
        region.bufferOffset = info.mip_ranges[mip].offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        region.imageExtent = {info.mip_ranges[mip].width, info.mip_ranges[mip].height, 1};
        vkCmdCopyImageToBuffer(batch.native(), image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            output->native(), 1, &region);
    }
    image.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    image.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    image.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    image.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    std::swap(image.oldLayout, image.newLayout);
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = output->native();
    host.size = VK_WHOLE_SIZE;
    dependency.imageMemoryBarrierCount = 0;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &host;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    const auto ticket = take(std::move(batch).submit());
    CHECK(take(session.queue().wait(ticket, UINT64_MAX)));
    std::vector<std::byte> bytes(last.offset + last.size);
    CHECK(output->read(0, bytes));
    output.reset();
    return bytes;
}
