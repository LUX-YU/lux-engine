#include <lux/engine/render/utils/StagingUploadHelper.hpp>
#include <lux/engine/render/utils/StagingRingBuffer.hpp>
#include <vk_mem_alloc.h>

namespace lux::render
{

StagingAlloc allocStaging(VmaAllocator allocator, VkDeviceSize bytes,
                          std::vector<StagingBuffer>& batch)
{
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size  = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
              | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo info{};
    VkBuffer buf        = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    if (vmaCreateBuffer(allocator, &bci, &aci, &buf, &alloc, &info) != VK_SUCCESS)
        return {};

    batch.emplace_back(allocator, buf, alloc);
    return {buf, alloc, info.pMappedData, /*srcOffset=*/0};
}

StagingAlloc allocStaging(VmaAllocator allocator, VkDeviceSize bytes,
                          std::vector<StagingBuffer>& batch,
                          StagingRingBuffer& ring)
{
    // Fast path: try ring sub-allocation.
    auto sub = ring.suballocate(bytes);
    if (sub)
        return {sub.buffer, /*allocation=*/nullptr, sub.mapped, sub.offset};

    // Fallback: dedicated VMA buffer.
    return allocStaging(allocator, bytes, batch);
}

} // namespace lux::render

