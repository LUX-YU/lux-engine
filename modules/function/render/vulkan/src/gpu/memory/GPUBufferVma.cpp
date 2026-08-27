#include <lux/engine/render/gpu/memory/GPUBuffer.hpp>
#include <vk_mem_alloc.h>

namespace lux::render
{

    bool createGpuBufferVmaBuffer(
        VmaAllocator allocator,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        bool cpu_writable,
        VkBuffer* pBuffer,
        VmaAllocation* pAllocation,
        void** ppMapped
    )
    {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo ai{};
        ai.usage = cpu_writable ? VMA_MEMORY_USAGE_AUTO : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ai.flags = cpu_writable
                       ? (VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT)
                       : 0;

        VmaAllocationInfo info{};
        if (vmaCreateBuffer(allocator, &bi, &ai, pBuffer, pAllocation, &info) != VK_SUCCESS)
            return false;

        if (ppMapped)
            *ppMapped = info.pMappedData;
        return true;
    }

    void destroyGpuBufferVmaBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation)
    {
        if (buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(allocator, buffer, allocation);
    }

    void flushGpuBufferVmaAllocation(
        VmaAllocator allocator,
        VmaAllocation allocation,
        VkDeviceSize offset,
        VkDeviceSize size
    )
    {
        if (allocation != nullptr)
            (void)vmaFlushAllocation(allocator, allocation, offset, size);
    }

    void invalidateGpuBufferVmaAllocation(
        VmaAllocator allocator,
        VmaAllocation allocation,
        VkDeviceSize offset,
        VkDeviceSize size
    )
    {
        if (allocation != nullptr)
            (void)vmaInvalidateAllocation(allocator, allocation, offset, size);
    }

} // namespace lux::render
