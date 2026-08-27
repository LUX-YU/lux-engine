#pragma once

#include <vulkan/vulkan.h>

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;

struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

struct VmaPool_T;
using VmaPool = VmaPool_T*;

struct VmaAllocationCreateInfo;
struct VmaAllocationInfo;

struct VmaVirtualAllocation_T;
using VmaVirtualAllocation = VmaVirtualAllocation_T*;

struct VmaVirtualBlock_T;
using VmaVirtualBlock = VmaVirtualBlock_T*;

#ifdef __cplusplus
extern "C" {
#endif

VkResult
vmaFlushAllocation(VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize offset, VkDeviceSize size);

void
vmaDestroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation);

#ifdef __cplusplus
}
#endif
