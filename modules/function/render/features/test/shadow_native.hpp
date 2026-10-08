#pragma once
#define LUX_FUNCTION_DLL_DISABLE
#define LUX_ENGINE_FUNCTION_RENDER_FEATURES_DLL_DISABLE
#include <lux/engine/gapi/vk/vk.hpp>
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <unordered_set>

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
        IDLE,
        POOL,
        SET,
        ALLOCATE_COMMAND,
        BEGIN_COMMAND,
        END_COMMAND,
        FENCE,
        SUBMIT,
        WAIT,
        COUNT
    };

    inline EBoundary boundary{};
    inline unsigned skip{}, rejected{}, writes{};
    inline bool reject_scene_ring{};
    inline std::array<unsigned, static_cast<unsigned>(EBoundary::COUNT)> attempts{};
    inline std::unordered_set<VkBuffer> live_buffers;
    inline std::unordered_set<VkDescriptorPool> live_pools;

#if defined(LUX_HZB_NATIVE_FAULTS)
    inline std::unordered_set<VkCommandBuffer> live_commands;
    inline std::unordered_set<VkFence> live_fences;
    inline bool in_flight{};
#endif

    bool rejects(EBoundary attempted)
    {
        ++attempts[static_cast<unsigned>(attempted)];
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
        if (reject_scene_ring && info->size == 4 * 1024 * 1024)
        {
            ++rejected;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        if (rejects(EBoundary::BUFFER))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreateBuffer(allocator, info, allocation_info, buffer, allocation, mapping);
#if defined(LUX_SCENE_NATIVE_FAULTS)
        if (result == VK_SUCCESS)
        {
            assert(live_buffers.insert(*buffer).second);
        }
#endif
        if (result == VK_SUCCESS && mapping && rejects(EBoundary::MAPPING))
        {
            mapping->pMappedData = nullptr;
        }
        return result;
    }

    void destroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation)
    {
#if defined(LUX_SCENE_NATIVE_FAULTS)
        assert(live_buffers.erase(buffer) == 1);
#endif
        vmaDestroyBuffer(allocator, buffer, allocation);
    }

    VkResult createPool(
        VkDevice device,
        const VkDescriptorPoolCreateInfo* info,
        const VkAllocationCallbacks* callbacks,
        VkDescriptorPool* pool
    )
    {
        if (rejects(EBoundary::POOL))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkCreateDescriptorPool(device, info, callbacks, pool);
        if (result == VK_SUCCESS)
        {
            assert(live_pools.insert(*pool).second);
        }
        return result;
    }

    void destroyPool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks* callbacks)
    {
        assert(live_pools.erase(pool) == 1);
        vkDestroyDescriptorPool(device, pool, callbacks);
    }

    VkResult allocateSets(VkDevice device, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* sets)
    {
        if (rejects(EBoundary::SET))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return vkAllocateDescriptorSets(device, info, sets);
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
        const auto result = vkDeviceWaitIdle(device);
#if defined(LUX_HZB_NATIVE_FAULTS)
        if (result == VK_SUCCESS)
        {
            in_flight = false;
        }
#endif
        return result;
    }

#if defined(LUX_HZB_NATIVE_FAULTS)
    VkResult allocateCommand(VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out)
    {
        if (rejects(EBoundary::ALLOCATE_COMMAND))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkAllocateCommandBuffers(device, info, out);
        if (result == VK_SUCCESS)
        {
            for (unsigned i = 0; i < info->commandBufferCount; ++i)
            {
                assert(live_commands.insert(out[i]).second);
            }
        }
        return result;
    }

    void freeCommands(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer* buffers)
    {
        assert(!in_flight);
        for (unsigned i = 0; i < count; ++i)
        {
            assert(live_commands.erase(buffers[i]) == 1);
        }
        vkFreeCommandBuffers(device, pool, count, buffers);
    }

    VkResult beginCommand(VkCommandBuffer command, const VkCommandBufferBeginInfo* info)
    {
        return rejects(EBoundary::BEGIN_COMMAND) ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkBeginCommandBuffer(command, info);
    }

    VkResult endCommand(VkCommandBuffer command)
    {
        return rejects(EBoundary::END_COMMAND) ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkEndCommandBuffer(command);
    }

    VkResult createFence(
        VkDevice device,
        const VkFenceCreateInfo* info,
        const VkAllocationCallbacks* callbacks,
        VkFence* out
    )
    {
        if (rejects(EBoundary::FENCE))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkCreateFence(device, info, callbacks, out);
        if (result == VK_SUCCESS)
        {
            assert(live_fences.insert(*out).second);
        }
        return result;
    }

    void destroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks* callbacks)
    {
        assert(!in_flight && live_fences.erase(fence) == 1);
        vkDestroyFence(device, fence, callbacks);
    }

    VkResult submit(VkQueue queue, uint32_t count, const VkSubmitInfo* info, VkFence fence)
    {
        if (rejects(EBoundary::SUBMIT))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkQueueSubmit(queue, count, info, fence);
        if (result == VK_SUCCESS)
        {
            in_flight = true;
        }
        return result;
    }

    VkResult wait(VkDevice device, uint32_t count, const VkFence* fences, VkBool32 all, uint64_t timeout)
    {
        if (rejects(EBoundary::WAIT))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkWaitForFences(device, count, fences, all, timeout);
        if (result == VK_SUCCESS)
        {
            in_flight = false;
        }
        return result;
    }
#endif

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
#define vmaDestroyBuffer shadow_fault::destroyBuffer
#define vmaFlushAllocation shadow_fault::flush
#define vkDeviceWaitIdle shadow_fault::idle
#define vkUpdateDescriptorSets shadow_fault::update
#if defined(LUX_SCENE_NATIVE_FAULTS)
#define vkCreateDescriptorPool shadow_fault::createPool
#define vkDestroyDescriptorPool shadow_fault::destroyPool
#define vkAllocateDescriptorSets shadow_fault::allocateSets
#endif
#if defined(LUX_HZB_NATIVE_FAULTS)
#define vkAllocateCommandBuffers shadow_fault::allocateCommand
#define vkFreeCommandBuffers shadow_fault::freeCommands
#define vkBeginCommandBuffer shadow_fault::beginCommand
#define vkEndCommandBuffer shadow_fault::endCommand
#define vkCreateFence shadow_fault::createFence
#define vkDestroyFence shadow_fault::destroyFence
#define vkQueueSubmit shadow_fault::submit
#define vkWaitForFences shadow_fault::wait
#endif
#include "../../vulkan/src/gpu/memory/VmaTypes.cpp"
#include "../../vulkan/src/gpu/VulkanContext.cpp"
#if defined(LUX_SCENE_NATIVE_FAULTS)
#include "../../vulkan/src/gpu/lifecycle/DeferredDestroyQueue.cpp"
#include "../../vulkan/src/gpu/memory/GPUBufferVma.cpp"
#include "../../vulkan/src/gpu/memory/StagingBuffer.cpp"
#include "../../vulkan/src/gpu/utils/StagingRingBuffer.cpp"
#include "../../vulkan/src/gpu/transfer/TransferScheduler.cpp"
#include "../../vulkan/src/gpu/RenderContext.cpp"
#include "../../vulkan/src/gpu/descriptor/SceneDescriptorArena.cpp"
#include "../../vulkan/src/gpu/descriptor/SceneDomainDescriptorSets.cpp"
#include "../../vulkan/src/resources/SceneResources.cpp"
#include "../../vulkan/src/scene/SceneGraphCache.cpp"
#include "../../vulkan/src/scene/SceneViewSet.cpp"
#include "../../vulkan/src/scene/RenderScene.cpp"
#include "../../vulkan/src/renderer/Renderer.cpp"
#if defined(LUX_HZB_NATIVE_FAULTS)
#include "../../vulkan/src/resources/hzb/HzbResources.cpp"
#include "../src/renderer/features/hzb/HzbFeature.cpp"
#endif
#undef vkCreateDescriptorPool
#undef vkDestroyDescriptorPool
#undef vkAllocateDescriptorSets
#endif
#include "../../vulkan/src/resources/lighting/ShadowResources.cpp"
#include "../src/renderer/features/shadow/EVSMShadowResources.cpp"
#include "../src/renderer/features/shadow/EVSMShadowTechnique.cpp"
#include "../src/renderer/features/shadow/ShadowMapFeature.cpp"
#include "../src/renderer/features/light/LightFeature.cpp"
#undef vmaCreateImage
#undef vkCreateImageView
#undef vmaCreateBuffer
#undef vmaDestroyBuffer
#undef vmaFlushAllocation
#undef vkDeviceWaitIdle
#undef vkUpdateDescriptorSets
#if defined(LUX_HZB_NATIVE_FAULTS)
#undef vkAllocateCommandBuffers
#undef vkFreeCommandBuffers
#undef vkBeginCommandBuffer
#undef vkEndCommandBuffer
#undef vkCreateFence
#undef vkDestroyFence
#undef vkQueueSubmit
#undef vkWaitForFences
#endif
#undef NDEBUG
#include <cassert>
// clang-format on
