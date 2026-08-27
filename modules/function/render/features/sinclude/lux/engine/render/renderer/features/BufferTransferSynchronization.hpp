#pragma once

#include <array>
#include <cstddef>
#include <vulkan/vulkan.h>

namespace lux::render
{
    /// Orders persistent/per-frame-slot buffer consumers from an earlier frame
    /// before vkCmdFillBuffer/vkCmdUpdateBuffer overwrites the same allocation.
    /// A host fence wait proves completion, but does not replace this device
    /// memory dependency in the new command buffer.
    template <std::size_t N>
    inline void
    synchronizeBeforeBufferTransferWrites(VkCommandBuffer command_buffer, const std::array<VkBuffer, N>& buffers)
    {
        std::array<VkBufferMemoryBarrier2, N> barriers{};
        std::uint32_t count = 0u;
        for (const VkBuffer buffer : buffers)
        {
            if (buffer == VK_NULL_HANDLE)
                continue;
            auto& barrier = barriers[count++];
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = buffer;
            barrier.offset = 0u;
            barrier.size = VK_WHOLE_SIZE;
        }
        if (count == 0u)
            return;

        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = count;
        dependency.pBufferMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(command_buffer, &dependency);
    }
} // namespace lux::render
