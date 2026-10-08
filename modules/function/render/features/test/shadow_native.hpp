#pragma once
#define LUX_FUNCTION_DLL_DISABLE
#define LUX_ENGINE_FUNCTION_RENDER_FEATURES_DLL_DISABLE
#include <lux/engine/gapi/vk/vk.hpp>
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <cassert>
#include <cstdint>

namespace shadow_fault
{
    enum class EBoundary
    {
        NONE,
        IMAGE,
        VIEW,
        BUFFER,
        MAPPING,
        FLUSH,
        IDLE
    };

    inline EBoundary boundary{};
    inline unsigned skip{}, rejected{}, writes{};

    bool rejects(EBoundary attempted)
    {
        if (boundary != attempted)
        {
            return false;
        }
        if (skip != 0)
        {
            --skip;
            return false;
        }
        ++rejected;
        return true;
    }

    VkResult createImage(
        VmaAllocator allocator,
        const VkImageCreateInfo* info,
        const VmaAllocationCreateInfo* allocation_info,
        VkImage* image,
        VmaAllocation* allocation,
        VmaAllocationInfo* mapping
    )
    {
        if (rejects(EBoundary::IMAGE))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return vmaCreateImage(allocator, info, allocation_info, image, allocation, mapping);
    }

    VkResult createView(
        VkDevice device,
        const VkImageViewCreateInfo* info,
        const VkAllocationCallbacks* callbacks,
        VkImageView* view
    )
    {
        if (rejects(EBoundary::VIEW))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return vkCreateImageView(device, info, callbacks, view);
    }

    VkResult createBuffer(
        VmaAllocator allocator,
        const VkBufferCreateInfo* info,
        const VmaAllocationCreateInfo* allocation_info,
        VkBuffer* buffer,
        VmaAllocation* allocation,
        VmaAllocationInfo* mapping
    )
    {
        if (rejects(EBoundary::BUFFER))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreateBuffer(allocator, info, allocation_info, buffer, allocation, mapping);
        if (result == VK_SUCCESS && mapping && rejects(EBoundary::MAPPING))
        {
            mapping->pMappedData = nullptr;
        }
        return result;
    }

    VkResult flush(VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize offset, VkDeviceSize bytes)
    {
        if (rejects(EBoundary::FLUSH))
        {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }
        return vmaFlushAllocation(allocator, allocation, offset, bytes);
    }

    VkResult idle(VkDevice device)
    {
        if (rejects(EBoundary::IDLE))
        {
            return VK_ERROR_DEVICE_LOST;
        }
        return vkDeviceWaitIdle(device);
    }

    void update(
        VkDevice device,
        uint32_t count,
        const VkWriteDescriptorSet* updates,
        uint32_t copies,
        const VkCopyDescriptorSet* copied
    )
    {
        writes += count;
        vkUpdateDescriptorSets(device, count, updates, copies, copied);
    }
} // namespace shadow_fault

// Compile the actual providers; only native boundaries are intercepted.
// clang-format off
#define NDEBUG
#include <cassert>
#define vmaCreateImage shadow_fault::createImage
#define vkCreateImageView shadow_fault::createView
#define vmaCreateBuffer shadow_fault::createBuffer
#define vmaFlushAllocation shadow_fault::flush
#define vkDeviceWaitIdle shadow_fault::idle
#define vkUpdateDescriptorSets shadow_fault::update
#include "../../vulkan/src/gpu/memory/VmaTypes.cpp"
#include "../../vulkan/src/gpu/VulkanContext.cpp"
#include "../../vulkan/src/resources/lighting/ShadowResources.cpp"
#include "../src/renderer/features/shadow/EVSMShadowResources.cpp"
#include "../src/renderer/features/shadow/EVSMShadowTechnique.cpp"
#include "../src/renderer/features/shadow/ShadowMapFeature.cpp"
#include "../src/renderer/features/light/LightFeature.cpp"
#undef vmaCreateImage
#undef vkCreateImageView
#undef vmaCreateBuffer
#undef vmaFlushAllocation
#undef vkDeviceWaitIdle
#undef vkUpdateDescriptorSets
#undef NDEBUG
#include <cassert>
// clang-format on
