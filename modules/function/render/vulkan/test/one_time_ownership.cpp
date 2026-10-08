#include <lux/engine/gapi/vk/vk.hpp>
#include <lux/engine/render/gpu/VmaFwd.hpp>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <type_traits>
#include <vector>

namespace
{
    enum class EFailure
    {
        NONE,
        POOL,
        SET,
        IMAGE,
        VIEW,
        SAMPLER,
        BUFFER,
        MAPPED,
        FLUSH,
        ALLOCATE,
        BEGIN,
        END,
        FENCE,
        SUBMIT,
        WAIT,
        ADDRESS,
        LAYOUT,
        DEVICE_IDLE
    };

    struct CommandOrigin
    {
        VkDevice device{};
        VkCommandPool pool{};
    };

    std::map<VkCommandBuffer, CommandOrigin> commands;
    std::map<VkFence, VkDevice> fences;
    std::map<VkDescriptorPool, VkDevice> pools;
    std::map<VkDescriptorSet, VkDescriptorPool> sets;
    std::map<VkImage, std::pair<VmaAllocator, VmaAllocation>> images;
    std::map<VkImageView, std::pair<VkDevice, VkImage>> views;
    std::map<VkSampler, VkDevice> samplers;
    std::map<VkBuffer, std::pair<VmaAllocator, VmaAllocation>> buffers;
    EFailure failure{};
    unsigned rejections{}, idle_calls{}, copied_descriptors{}, skip_rejections{};
    bool submitted{};
    unsigned acquisition{}, fail_acquisition{};
    bool measure_acquisitions{};
    EFailure rejected_boundary{};
    VkResult set_failure_result{VK_ERROR_OUT_OF_POOL_MEMORY};
    bool reject_set_once{};

    bool reject(EFailure boundary)
    {
        if (measure_acquisitions)
        {
            ++acquisition;
            if (acquisition == fail_acquisition)
            {
                ++rejections;
                rejected_boundary = boundary;
                return true;
            }
        }
        if (failure != boundary)
        {
            return false;
        }
        if (skip_rejections != 0)
        {
            --skip_rejections;
            return false;
        }
        ++rejections;
        return true;
    }

    VkResult trackedCreateStagingBuffer(
        VmaAllocator allocator,
        const VkBufferCreateInfo* info,
        const VmaAllocationCreateInfo* allocation_info,
        VkBuffer* out,
        VmaAllocation* allocation,
        VmaAllocationInfo* mapped
    )
    {
        if (reject(EFailure::BUFFER))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreateBuffer(allocator, info, allocation_info, out, allocation, mapped);
        if (result == VK_SUCCESS)
        {
            assert(buffers.emplace(*out, std::pair{allocator, *allocation}).second);
            const bool requested_mapping = (allocation_info->flags & VMA_ALLOCATION_CREATE_MAPPED_BIT) != 0;
            if (mapped && requested_mapping && reject(EFailure::MAPPED))
            {
                mapped->pMappedData = nullptr;
            }
        }
        return result;
    }

    void trackedDestroyStagingBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation)
    {
        assert((buffers.at(buffer) == std::pair{allocator, allocation}));
        assert(!submitted);
        buffers.erase(buffer);
        vmaDestroyBuffer(allocator, buffer, allocation);
    }

    VkResult trackedFlushStagingAllocation(
        VmaAllocator allocator,
        VmaAllocation allocation,
        VkDeviceSize offset,
        VkDeviceSize size
    )
    {
        if (reject(EFailure::FLUSH))
        {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }
        return vmaFlushAllocation(allocator, allocation, offset, size);
    }

    unsigned layout_creations{}, layout_destroys{};

    VkResult createDescriptorLayout(
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo* info,
        const VkAllocationCallbacks* callbacks,
        VkDescriptorSetLayout* out
    )
    {
        if (reject(EFailure::LAYOUT))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkCreateDescriptorSetLayout(device, info, callbacks, out);
        if (result == VK_SUCCESS)
        {
            ++layout_creations;
        }
        return result;
    }

    void destroyDescriptorLayout(VkDevice device, VkDescriptorSetLayout layout, const VkAllocationCallbacks* callbacks)
    {
        ++layout_destroys;
        vkDestroyDescriptorSetLayout(device, layout, callbacks);
    }

    VkResult createPool(
        VkDevice device,
        const VkDescriptorPoolCreateInfo* info,
        const VkAllocationCallbacks* a,
        VkDescriptorPool* out
    )
    {
        *out = VK_NULL_HANDLE;
        if (reject(EFailure::POOL))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkCreateDescriptorPool(device, info, a, out);
        if (result == VK_SUCCESS)
        {
            assert(pools.emplace(*out, device).second);
        }
        return result;
    }

    void destroyPool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks* a)
    {
        assert(pools.at(pool) == device);
        pools.erase(pool);
        std::erase_if(sets, [pool](const auto& value) { return value.second == pool; });
        vkDestroyDescriptorPool(device, pool, a);
    }

    VkResult allocateSets(VkDevice device, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* out)
    {
        if (reject(EFailure::SET))
        {
            if (reject_set_once)
            {
                reject_set_once = false;
                failure = EFailure::NONE;
            }
            return set_failure_result;
        }
        const auto result = vkAllocateDescriptorSets(device, info, out);
        if (result == VK_SUCCESS)
        {
            for (unsigned i = 0; i < info->descriptorSetCount; ++i)
            {
                assert(sets.emplace(out[i], info->descriptorPool).second);
            }
        }
        return result;
    }

    VkResult freeSets(VkDevice device, VkDescriptorPool pool, std::uint32_t count, const VkDescriptorSet* handles)
    {
        assert(pools.at(pool) == device);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            assert(sets.at(handles[i]) == pool);
            sets.erase(handles[i]);
        }
        return vkFreeDescriptorSets(device, pool, count, handles);
    }

    unsigned descriptor_writes{};

    struct BufferDescriptorWrite
    {
        VkDevice device;
        VkDescriptorSet set;
        std::uint32_t binding;
        std::uint32_t index;
        VkBuffer buffer;
        VkDeviceSize offset;
    };

    bool trace_buffer_writes{};
    std::vector<BufferDescriptorWrite> buffer_writes;

    void updateDescriptors(
        VkDevice device,
        std::uint32_t write_count,
        const VkWriteDescriptorSet* writes,
        std::uint32_t copy_count,
        const VkCopyDescriptorSet* copies
    )
    {
        descriptor_writes += write_count;
        if (trace_buffer_writes)
        {
            for (std::uint32_t i = 0; i < write_count; ++i)
            {
                const auto& write = writes[i];
                assert(write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
                assert(write.descriptorCount == 1 && write.pBufferInfo);
                buffer_writes.push_back(
                    {device,
                     write.dstSet,
                     write.dstBinding,
                     write.dstArrayElement,
                     write.pBufferInfo->buffer,
                     write.pBufferInfo->offset}
                );
            }
        }
        for (std::uint32_t i = 0; i < copy_count; ++i)
        {
            copied_descriptors += copies[i].descriptorCount;
        }
        vkUpdateDescriptorSets(device, write_count, writes, copy_count, copies);
    }

    VkResult createImage(
        VmaAllocator allocator,
        const VkImageCreateInfo* info,
        const VmaAllocationCreateInfo* allocation_info,
        VkImage* image,
        VmaAllocation* allocation,
        VmaAllocationInfo* result_info
    )
    {
        *image = VK_NULL_HANDLE;
        *allocation = nullptr;
        if (reject(EFailure::IMAGE))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreateImage(allocator, info, allocation_info, image, allocation, result_info);
        if (result == VK_SUCCESS)
        {
            assert(images.emplace(*image, std::pair{allocator, *allocation}).second);
        }
        return result;
    }

    void destroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation)
    {
        assert(!submitted);
        assert(images.at(image) == std::pair(allocator, allocation));
        for (const auto& [view, origin] : views)
        {
            assert(origin.second != image);
        }
        images.erase(image);
        vmaDestroyImage(allocator, image, allocation);
    }

    VkResult createView(
        VkDevice device,
        const VkImageViewCreateInfo* info,
        const VkAllocationCallbacks* a,
        VkImageView* out
    )
    {
        *out = VK_NULL_HANDLE;
        if (reject(EFailure::VIEW))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkCreateImageView(device, info, a, out);
        if (result == VK_SUCCESS)
        {
            assert(views.emplace(*out, std::pair{device, info->image}).second);
        }
        return result;
    }

    void destroyView(VkDevice device, VkImageView view, const VkAllocationCallbacks* a)
    {
        if (view == VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, view, a);
            return;
        }
        assert(!submitted && views.at(view).first == device);
        views.erase(view);
        vkDestroyImageView(device, view, a);
    }

    VkResult createSampler(
        VkDevice device,
        const VkSamplerCreateInfo* info,
        const VkAllocationCallbacks* a,
        VkSampler* out
    )
    {
        *out = VK_NULL_HANDLE;
        if (reject(EFailure::SAMPLER))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkCreateSampler(device, info, a, out);
        if (result == VK_SUCCESS)
        {
            assert(samplers.emplace(*out, device).second);
        }
        return result;
    }

    void destroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks* a)
    {
        assert(!submitted && samplers.at(sampler) == device);
        samplers.erase(sampler);
        vkDestroySampler(device, sampler, a);
    }

    VkResult allocate(VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out)
    {
        if (reject(EFailure::ALLOCATE))
        {
            std::fill_n(out, info->commandBufferCount, VK_NULL_HANDLE);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkAllocateCommandBuffers(device, info, out);
        if (result == VK_SUCCESS)
        {
            for (unsigned i = 0; i < info->commandBufferCount; ++i)
            {
                assert(commands.emplace(out[i], CommandOrigin{device, info->commandPool}).second);
            }
        }
        return result;
    }

    void freeCommands(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer* buffers)
    {
        assert(!submitted);
        for (unsigned i = 0; i < count; ++i)
        {
            const auto found = commands.find(buffers[i]);
            assert(found != commands.end());
            assert(found->second.device == device && found->second.pool == pool);
            commands.erase(found);
        }
        vkFreeCommandBuffers(device, pool, count, buffers);
    }

    VkResult begin(VkCommandBuffer command, const VkCommandBufferBeginInfo* info)
    {
        return reject(EFailure::BEGIN) ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkBeginCommandBuffer(command, info);
    }

    VkResult end(VkCommandBuffer command)
    {
        return reject(EFailure::END) ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkEndCommandBuffer(command);
    }

    VkResult createFence(VkDevice device, const VkFenceCreateInfo* info, const VkAllocationCallbacks* a, VkFence* out)
    {
        *out = VK_NULL_HANDLE;
        if (reject(EFailure::FENCE))
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkCreateFence(device, info, a, out);
        if (result == VK_SUCCESS)
        {
            assert(fences.emplace(*out, device).second);
        }
        return result;
    }

    void destroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks* a)
    {
        assert(!submitted);
        const auto found = fences.find(fence);
        assert(found != fences.end() && found->second == device);
        fences.erase(found);
        vkDestroyFence(device, fence, a);
    }

    VkResult submit(VkQueue queue, uint32_t count, const VkSubmitInfo* info, VkFence fence)
    {
        const auto result =
            reject(EFailure::SUBMIT) ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkQueueSubmit(queue, count, info, fence);
        submitted = result == VK_SUCCESS;
        return result;
    }

    VkResult wait(VkDevice device, uint32_t count, const VkFence* fs, VkBool32 all, uint64_t timeout)
    {
        // Complete actual GPU work before injecting an error, without pretending that
        // production code can infer completion from that error. It must still wait idle.
        const auto result = vkWaitForFences(device, count, fs, all, timeout);
        assert(result == VK_SUCCESS);
        if (reject(EFailure::WAIT))
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        submitted = false;
        return result;
    }

    VkDeviceAddress trackedBufferAddress(VkDevice device, const VkBufferDeviceAddressInfo* info)
    {
        return reject(EFailure::ADDRESS) ? 0 : vkGetBufferDeviceAddress(device, info);
    }

    VkResult trackedDeviceWaitIdle(VkDevice device)
    {
        ++idle_calls;
        if (reject(EFailure::DEVICE_IDLE))
            return VK_ERROR_DEVICE_LOST;
        const auto result = vkDeviceWaitIdle(device);
        if (result == VK_SUCCESS)
        {
            submitted = false;
        }
        return result;
    }
} // namespace

// Actual allocation and descriptor-set implementation; native faults only.
// clang-format off
#define vkCreateDescriptorSetLayout createDescriptorLayout
#define vkDestroyDescriptorSetLayout destroyDescriptorLayout
#define vmaCreateBuffer trackedCreateStagingBuffer
#define vmaDestroyBuffer trackedDestroyStagingBuffer
#define vmaFlushAllocation trackedFlushStagingAllocation
#define vmaCreateImage createImage
#define vmaDestroyImage destroyImage
#define vkCreateDescriptorPool createPool
#define vkDestroyDescriptorPool destroyPool
#define vkUpdateDescriptorSets updateDescriptors
#define vkAllocateDescriptorSets allocateSets
#define vkFreeDescriptorSets freeSets
#define vkCreateImageView createView
#define vkDestroyImageView destroyView
#define vkCreateSampler createSampler
#define vkDestroySampler destroySampler
// Production code uses the shipping NDEBUG policy; interception and test assertions remain active.
#define NDEBUG
#include <cassert>
#include "../src/gpu/memory/VmaTypes.cpp"
#include "../src/gpu/memory/StagingBuffer.cpp"
#define vkAllocateCommandBuffers allocate
#define vkFreeCommandBuffers freeCommands
#define vkBeginCommandBuffer begin
#define vkEndCommandBuffer end
#define vkCreateFence createFence
#define vkDestroyFence destroyFence
#define vkQueueSubmit submit
#define vkWaitForFences wait
#define vkDeviceWaitIdle trackedDeviceWaitIdle
#include "../src/gpu/VulkanContext.cpp"
#undef vkDeviceWaitIdle
#include "../src/gpu/pipeline/GeneralDescriptorSetLayout.cpp"
#include "../src/resources/descriptor/BindlessCombinedSet.cpp"
#include "../src/gpu/memory/GPUBufferVma.cpp"
#include "../src/resources/TextureResources.cpp"
#include "../src/resources/material/MaterialResources.cpp"
#include "../src/gpu/descriptor/DescriptorService.cpp"
#include "../src/gpu/descriptor/SceneDescriptorArena.cpp"
#include "../../features/src/renderer/features/canvas2d/Canvas2DInstanceArena.cpp"
#include "../src/gpu/descriptor/SceneDomainDescriptorSets.cpp"
#include "../src/resources/SceneResources.cpp"
#include <lux/engine/render/gpu/memory/PagedGpuStream.hpp>
#include "../src/resources/mesh/MeshSectionTable.cpp"
#define vkGetBufferDeviceAddress trackedBufferAddress
#include "../src/resources/mesh/SparseInstanceStream.cpp"
#undef vkGetBufferDeviceAddress
#include "../src/resources/mesh/InstanceResources.cpp"
#include "../src/resources/mesh/InstanceSlotRegistry.cpp"
#include "../src/resources/mesh/MdcTable.cpp"
#include "../src/resources/lighting/LightResources.cpp"
#include "../src/resources/lighting/ShadowResources.cpp"
#include "../src/resources/hzb/HzbResources.cpp"
#include "../src/resources/vertex/VertexPoolRegistry.cpp"
#include "../src/resources/vertex/TransientVertexSource.cpp"
#include "../src/resources/vertex/SkinningResources.cpp"
#include <lux/engine/render/gpu/lifecycle/ResourceRegistry.hpp>
#include "../src/gpu/lifecycle/DeferredDestroyQueue.cpp"
#undef vkWaitForFences
#undef vkQueueSubmit
#undef vkDestroyFence
#undef vkCreateFence
#undef vkEndCommandBuffer
#undef vkBeginCommandBuffer
#undef vkFreeCommandBuffers
#undef vkAllocateCommandBuffers
#undef vkCreateDescriptorPool
#undef vkDestroyDescriptorPool
#undef vkUpdateDescriptorSets
#undef vkFreeDescriptorSets
#undef vkAllocateDescriptorSets
#undef vkCreateDescriptorSetLayout
#undef vkDestroyDescriptorSetLayout
#undef vmaCreateBuffer
#undef vmaDestroyBuffer
#undef vmaFlushAllocation
#undef vmaCreateImage
#undef vmaDestroyImage
#undef vkCreateImageView
#undef vkDestroyImageView
#undef vkCreateSampler
#undef vkDestroySampler
// clang-format on
#undef NDEBUG
#include <cassert>

void checkSceneDescriptorArena(lux::render::DeviceContext& device, VkDescriptorSetLayout layout)
{
    using namespace lux::render;
    static_assert(!std::is_default_constructible_v<SceneDescriptorArena>);
    static_assert(!std::is_copy_constructible_v<SceneDescriptorArena>);
    static_assert(!std::is_move_constructible_v<SceneDescriptorArena>);
    const auto original_pools = pools.size();
    const auto original_sets = sets.size();
    auto no_device = SceneDescriptorArena::create(VK_NULL_HANDLE, {});
    assert(!no_device && isError<err::internal::InvalidArgument>(no_device.error()));
    SceneDescriptorArena::PoolSizeTemplate empty{};
    empty.max_sets = 0;
    auto no_capacity = SceneDescriptorArena::create(device.logicalDevice(), empty);
    assert(!no_capacity && isError<err::internal::InvalidArgument>(no_capacity.error()));
    empty.max_sets = 1;
    empty.storage_buffer = empty.combined_image_sampler = empty.uniform_buffer = 0;
    auto no_descriptors = SceneDescriptorArena::create(device.logicalDevice(), empty);
    assert(!no_descriptors && isError<err::internal::InvalidArgument>(no_descriptors.error()));
    auto candidate = SceneDescriptorArena::create(device.logicalDevice(), {});
    assert(candidate && pools.size() == original_pools && sets.size() == original_sets);
    auto& arena = **candidate;
    const auto no_layout = arena.allocate(VK_NULL_HANDLE);
    assert(!no_layout && isError<err::internal::InvalidArgument>(no_layout.error()));
    failure = EFailure::POOL;
    const auto failed_pool = arena.allocate(layout);
    assert(!failed_pool && isError<err::device::VulkanCallFailed>(failed_pool.error()));
    assert(failed_pool.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
    assert(arena.poolCount() == 0 && pools.size() == original_pools && sets.size() == original_sets);
    failure = EFailure::SET;
    const auto failed_set = arena.allocate(layout);
    assert(!failed_set && isError<err::device::VulkanCallFailed>(failed_set.error()));
    assert(failed_set.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_POOL_MEMORY));
    assert(arena.poolCount() == 0 && pools.size() == original_pools && sets.size() == original_sets);
    failure = EFailure::NONE;
    const auto first = arena.allocate(layout);
    assert(first && *first != VK_NULL_HANDLE && arena.poolCount() == 1);
    const auto first_pool = sets.at(*first);

    failure = EFailure::SET;
    set_failure_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const auto before_failure = rejections;
    const auto no_growth = arena.allocate(layout);
    assert(!no_growth && no_growth.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
    assert(rejections == before_failure + 1 && arena.poolCount() == 1);
    assert(sets.at(*first) == first_pool && pools.size() == original_pools + 1);

    set_failure_result = VK_ERROR_FRAGMENTED_POOL;
    const auto before_retry = rejections;
    const auto failed_growth = arena.allocate(layout);
    assert(!failed_growth && failed_growth.error().args[0] == encodeVkResult(VK_ERROR_FRAGMENTED_POOL));
    assert(rejections == before_retry + 2 && arena.poolCount() == 1);
    assert(pools.size() == original_pools + 1 && sets.size() == original_sets + 1);
    assert(sets.at(*first) == first_pool);

    reject_set_once = true;
    const auto grown = arena.allocate(layout);
    assert(grown && *grown != VK_NULL_HANDLE && arena.poolCount() == 2);
    assert(sets.at(*grown) != first_pool && sets.at(*first) == first_pool);
    assert(arena.beginGeneration() == 2 && arena.generation() == 1);
    assert(arena.poolCount() == 0 && arena.retiredPoolCount() == 2);
    assert(sets.contains(*first) && sets.contains(*grown));
    const auto current = arena.allocate(layout);
    assert(current && arena.poolCount() == 1 && arena.retiredPoolCount() == 2);
    arena.releaseRetired();
    assert(!sets.contains(*first) && !sets.contains(*grown) && sets.contains(*current));
    assert(arena.retiredPoolCount() == 0 && pools.size() == original_pools + 1);
    candidate->reset();
    assert(pools.size() == original_pools && sets.size() == original_sets);
    set_failure_result = VK_ERROR_OUT_OF_POOL_MEMORY;
    std::puts("Scene descriptor arena: native allocation errors, bounded growth, retry and generation retirement PASS");
}

void checkHzbDescriptorFailure(lux::render::DeviceContext& device)
{
    using namespace lux::render;
    const std::array bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}
    };
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout_info.pBindings = bindings.data();
    auto layout = DescriptorSetLayoutOwner::create(device.logicalDevice(), layout_info);
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    auto sampler = SamplerOwner::create(device.logicalDevice(), sampler_info);
    auto arena = SceneDescriptorArena::create(device.logicalDevice(), {});
    assert(layout && sampler && arena);
    DeferredDestroyQueue retirement;
    retirement.init(device.vmaAllocator(), device.logicalDevice());
    retirement.beginFrame(7);
    const auto original_images = images.size();
    const auto original_views = views.size();
    const auto original_buffers = buffers.size();
    const auto original_sets = sets.size();
    {
        HzbResources hzb;
        assert(hzb.init(
            {.device = device.logicalDevice(),
             .allocator = device.vmaAllocator(),
             .arena = arena->get(),
             .read_layout = layout->get(),
             .sampler = sampler->get(),
             .deferred_queue = &retirement}
        ));
        failure = EFailure::SET;
        set_failure_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        const auto rejected = hzb.ensureView(17, 8, 8);
        assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
        assert(rejected.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        assert(!hzb.viewReady(17) && (*arena)->poolCount() == 0 && sets.size() == original_sets);
        assert(retirement.pendingCount() != 0);
        retirement.collect(6);
        assert(images.size() > original_images && buffers.size() > original_buffers);
        retirement.collect(7);
        assert(
            images.size() == original_images && views.size() == original_views && buffers.size() == original_buffers
        );
        failure = EFailure::NONE;
        set_failure_result = VK_ERROR_OUT_OF_POOL_MEMORY;
        assert(hzb.ensureView(17, 8, 8) && hzb.viewReady(17));
        assert(sets.size() == original_sets + 2 && (*arena)->poolCount() == 1);
        assert(hzb.ensureView(17, 8, 8) && sets.size() == original_sets + 2);
        hzb.evictView(17);
        assert(!hzb.viewReady(17) && retirement.pendingCount() != 0);
        retirement.collect(7);
    }
    assert(images.size() == original_images && views.size() == original_views && buffers.size() == original_buffers);
    arena->reset();
    assert(sets.size() == original_sets && retirement.pendingCount() == 0);
    std::puts("HZB descriptor failure: exact allocation error, no ready view, retry and original retirement PASS");
}

void checkSparseStorage(lux::render::DeviceContext& device, lux::render::DeferredDestroyQueue& retirement)
{
    using namespace lux::render;
    using Storage = SparseInstanceStreamStorage;
    using Stream = TSparseInstanceStream<std::uint32_t>;
    static_assert(!std::is_default_constructible_v<Storage>);
    static_assert(!std::is_copy_constructible_v<Storage>);
    static_assert(std::is_nothrow_move_constructible_v<Storage>);
    static_assert(!std::is_move_assignable_v<Storage>);
    static_assert(!std::is_default_constructible_v<Stream>);
    static_assert(!std::is_copy_constructible_v<Stream>);
    static_assert(std::is_nothrow_move_constructible_v<Stream>);
    retirement.flushAll();
    assert(buffers.empty());
    const auto assert_native_error = [](const RenderError& error)
    {
        assert(isError<err::device::VulkanCallFailed>(error));
        assert(error.args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
    };
    for (bool sparse : {false, true})
    {
        for (auto capacity : {0u, UINT32_MAX})
        {
            auto rejected = Stream::create(device, retirement, capacity, sparse);
            assert(!rejected && isError<err::internal::InvalidArgument>(rejected.error()));
            assert(buffers.empty() && retirement.pendingCount() == 0);
        }
        auto bad_stride = Storage::create(device, retirement, 0, 1, sparse);
        assert(!bad_stride && isError<err::internal::InvalidArgument>(bad_stride.error()));
        // Every mandatory allocation in the initial three-page extent, including a nonempty prefix.
        const unsigned acquisitions = sparse ? 3 : 1;
        for (unsigned index = 0; index < acquisitions; ++index)
        {
            failure = EFailure::BUFFER;
            skip_rejections = index;
            auto rejected = Stream::create(device, retirement, 2 * kInstanceSlotsPerPage + 1, sparse);
            assert(!rejected);
            assert_native_error(rejected.error());
            assert(buffers.empty() && retirement.pendingCount() == 0);
        }
        if (sparse)
        {
            for (unsigned index = 0; index < 3; ++index)
            {
                failure = EFailure::ADDRESS;
                skip_rejections = index;
                auto rejected = Stream::create(device, retirement, 2 * kInstanceSlotsPerPage + 1, true);
                assert(!rejected && isError<err::memory::BufferDeviceAddressUnavailable>(rejected.error()));
                assert(buffers.empty() && retirement.pendingCount() == 0);
            }
        }
        failure = EFailure::NONE;
        retirement.beginFrame(37);
        {
            auto candidate = Stream::create(device, retirement, 1, sparse);
            assert(candidate);
            Stream stream(std::move(*candidate));
            assert(stream.sparse() == sparse && stream.capacity() == kInstanceSlotsPerPage);
            assert(stream.pageCount() == 1 && buffers.size() == 1 && retirement.pendingCount() == 0);
            const auto first_buffer = stream.buffer();
            const auto first_address = stream.pageAddress(0);
            assert(sparse ? first_buffer == VK_NULL_HANDLE && first_address != 0 : first_buffer != VK_NULL_HANDLE);
            assert(stream.at(0) == 0 && stream.at(kInstanceSlotsPerPage - 1) == 0);
            stream.at(0) = 71;
            stream.at(kInstanceSlotsPerPage - 1) = 91;
            auto* stable_cpu = &stream.at(0);
            stream.markDirty(0);
            stream.markDirty(0);
            stream.markDirty(kInstanceSlotsPerPage - 1);
            for (unsigned index = 0; index < (sparse ? 2u : 1u); ++index)
            {
                failure = EFailure::BUFFER;
                skip_rejections = index;
                const auto rejected = stream.reserve(2 * kInstanceSlotsPerPage + 1);
                assert(!rejected);
                assert_native_error(rejected.error());
                assert(stream.pageCount() == 1 && stream.capacity() == kInstanceSlotsPerPage);
                assert(&stream.at(0) == stable_cpu && stream.at(0) == 71);
                assert(stream.at(kInstanceSlotsPerPage - 1) == 91 && stream.hasDirtyPages());
                assert(stream.buffer() == first_buffer && stream.pageAddress(0) == first_address);
                assert(buffers.size() == 1 && retirement.pendingCount() == 0);
            }
            if (sparse)
            {
                failure = EFailure::ADDRESS;
                skip_rejections = 1;
                assert(!stream.reserve(2 * kInstanceSlotsPerPage + 1));
                assert(buffers.size() == 1 && stream.pageCount() == 1 && stream.pageAddress(0) == first_address);
            }
            failure = EFailure::NONE;
            assert(!stream.reserve(UINT32_MAX));
            assert(stream.reserve(0) && stream.reserve(kInstanceSlotsPerPage));
            assert(buffers.size() == 1 && retirement.pendingCount() == 0);
            assert(stream.reserve(2 * kInstanceSlotsPerPage + 1));
            assert(stream.capacity() == 3 * kInstanceSlotsPerPage && stream.pageCount() == 3);
            assert(&stream.at(0) == stable_cpu && stream.at(0) == 71);
            assert(stream.at(kInstanceSlotsPerPage) == 0 && stream.at(3 * kInstanceSlotsPerPage - 1) == 0);
            if (sparse)
            {
                assert(stream.pageAddress(0) == first_address && stream.pageAddress(1) != first_address);
                assert(stream.pageAddress(2) != stream.pageAddress(1) && buffers.size() == 3);
            }
            else
            {
                assert(stream.buffer() != first_buffer && retirement.pendingCount() == 1);
                assert(buffers.size() == 2); // Prior published flat allocation is still at the watermark.
            }
            std::vector<Stream::UploadChunk> chunks;
            const auto dirty_bytes = stream.collectUploadChunks(stream.capacity(), false, chunks);
            assert(dirty_bytes == 2 * 512 * sizeof(std::uint32_t) && chunks.size() == 2);
            assert(chunks[0].src == reinterpret_cast<const std::byte*>(stable_cpu));
            assert(chunks[0].destination && chunks[0].destination_offset == 0);
            assert(chunks[1].destination == chunks[0].destination);
            assert(chunks[1].destination_offset == (kInstanceSlotsPerPage - 512) * sizeof(std::uint32_t));
            stream.clearDirtyState();
            assert(!stream.hasDirtyPages());
            chunks.clear();
            const auto full_bytes = stream.collectUploadChunks(2 * kInstanceSlotsPerPage + 7, true, chunks);
            assert(full_bytes == (2 * kInstanceSlotsPerPage + 7) * sizeof(std::uint32_t) && chunks.size() == 3);
            assert(chunks[2].size == 7 * sizeof(std::uint32_t));
            assert(chunks[2].destination_offset == (sparse ? 0 : 2 * kInstanceSlotsPerPage * sizeof(std::uint32_t)));
            assert(
                sparse ? chunks[0].destination != chunks[2].destination : chunks[0].destination == chunks[2].destination
            );
            // reserve is now a complete acceptance, not a provisional append followed by public rollback.
            // Aggregate rejection/prefix cleanup is exercised through actual InstanceResources below.
            stream.markDirty(2 * kInstanceSlotsPerPage);
            assert(stream.reserve(kInstanceSlotsPerPage + 1));
            assert(stream.pageCount() == 3 && stream.at(0) == 71 && stream.hasDirtyPages());
            assert(buffers.size() == (sparse ? 3u : 2u));
            assert(retirement.pendingCount() == (sparse ? 0u : 1u));
        }
        const auto retained = buffers.size();
        assert(retained > 0 && retirement.pendingCount() == retained);
        retirement.collect(36);
        assert(buffers.size() == retained);
        retirement.collect(37);
        assert(buffers.empty() && retirement.pendingCount() == 0);
    }
    std::puts(
        "Sparse/flat field storage: complete backing, exact failures, stable CPU pages, strong growth, acceptance "
        "and retirement PASS"
    );
}

void checkSparsePageTable(lux::render::DeviceContext& device, lux::render::DeferredDestroyQueue& retirement)
{
    using namespace lux::render;
    static_assert(!std::is_default_constructible_v<SparseInstancePageTable>);
    static_assert(!std::is_copy_constructible_v<SparseInstancePageTable>);
    static_assert(!std::is_move_constructible_v<SparseInstancePageTable>);
    const auto original_buffers = buffers.size();
    const auto original_pending = retirement.pendingCount();
    for (const auto boundary : {EFailure::BUFFER, EFailure::MAPPED, EFailure::FLUSH})
    {
        failure = boundary;
        auto rejected = SparseInstancePageTable::create(device, retirement);
        assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
        const auto native_error =
            boundary == EFailure::BUFFER ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_MEMORY_MAP_FAILED;
        assert(rejected.error().args[0] == encodeVkResult(native_error));
        assert(buffers.size() == original_buffers && retirement.pendingCount() == original_pending);
        failure = EFailure::NONE;
    }
    const auto mapped = [](VkBuffer buffer)
    {
        const auto& origin = buffers.at(buffer);
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(origin.first, origin.second, &info);
        return info.pMappedData;
    };
    const auto find_leaf = [&](VkDeviceAddress address)
    {
        for (const auto& [buffer, allocation] : buffers)
        {
            VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            info.buffer = buffer;
            if (vkGetBufferDeviceAddress(device.logicalDevice(), &info) == address)
            {
                return static_cast<GpuInstancePageAddresses*>(mapped(buffer));
            }
        }
        return static_cast<GpuInstancePageAddresses*>(nullptr);
    };
    retirement.beginFrame(23);
    {
        auto owner = SparseInstancePageTable::create(device, retirement);
        assert(owner && (*owner)->rootBuffer() && (*owner)->leafCount() == 0);
        auto* root = static_cast<VkDeviceAddress*>(mapped((*owner)->rootBuffer()));
        assert(std::all_of(root, root + kInstancePageTableAxisSize, [](auto address) { return address == 0; }));
        const GpuInstancePageAddresses first{11, 12, 13, 14};
        for (const auto boundary : {EFailure::BUFFER, EFailure::MAPPED, EFailure::FLUSH, EFailure::ADDRESS})
        {
            failure = boundary;
            auto rejected = (*owner)->publish(0, first);
            assert(!rejected && (*owner)->leafCount() == 0 && root[0] == 0);
            assert(buffers.size() == original_buffers + 1 && retirement.pendingCount() == original_pending);
            if (boundary == EFailure::ADDRESS)
            {
                assert(isError<err::memory::BufferDeviceAddressUnavailable>(rejected.error()));
            }
            else
            {
                assert(isError<err::device::VulkanCallFailed>(rejected.error()));
            }
            failure = EFailure::NONE;
        }
        failure = EFailure::FLUSH;
        skip_rejections = 1; // leaf is ready, but root publication fails
        auto rejected_root = (*owner)->publish(512, first);
        assert(!rejected_root && (*owner)->leafCount() == 0 && root[1] == 0);
        assert(buffers.size() == original_buffers + 1);
        failure = EFailure::NONE;
        assert((*owner)->publish(0, first));
        auto* leaf = find_leaf(root[0]);
        assert(leaf && leaf[0].transform == 11 && leaf[0].cull_meta == 14 && (*owner)->leafCount() == 1);
        const GpuInstancePageAddresses second{21, 22, 23, 24};
        failure = EFailure::FLUSH;
        auto rejected_update = (*owner)->publish(0, second);
        assert(!rejected_update && leaf[0].transform == 11 && leaf[0].cull_meta == 14);
        assert((*owner)->leafCount() == 1 && buffers.size() == original_buffers + 2);
        failure = EFailure::NONE;
        assert((*owner)->publish(511, second));
        assert(leaf[511].property == 23 && leaf[0].transform == 11 && (*owner)->leafCount() == 1);
        assert((*owner)->publish(512, second));
        assert((*owner)->publish(512 * 512 - 1, second));
        assert((*owner)->leafCount() == 3 && find_leaf(root[511])[511].previous_transform == 22);
        assert(!(*owner)->publish(512 * 512, first));
        assert((*owner)->leafCount() == 3);
    }
    assert(buffers.size() == original_buffers + 4 && retirement.pendingCount() == original_pending + 4);
    retirement.collect(22);
    assert(buffers.size() == original_buffers + 4);
    retirement.collect(23);
    assert(buffers.size() == original_buffers && retirement.pendingCount() == original_pending);
    std::puts(
        "Sparse page table: complete root, leaf publication rollback/retry, mapped identity and serial retirement PASS"
    );
}

void checkPagedCapacity(lux::render::DeviceContext& device, lux::render::DeferredDestroyQueue& retirement)
{
    using namespace lux::render;
    using Stream = TPagedGpuStream<uint32_t>;
    static_assert(std::is_same_v<decltype(std::declval<Stream&>().cpuData()), std::span<uint32_t>>);
    static_assert(!std::is_default_constructible_v<Stream>);
    static_assert(!std::is_copy_constructible_v<Stream>);
    static_assert(std::is_nothrow_move_constructible_v<Stream>);
    static_assert(std::is_nothrow_move_assignable_v<Stream>);
    static_assert(!std::is_default_constructible_v<MeshSectionTable>);
    static_assert(!std::is_copy_constructible_v<MeshSectionTable>);
    failure = EFailure::BUFFER;
    auto rejected_stream = Stream::create(device, retirement, 8);
    auto rejected_table = MeshSectionTable::create(device, retirement, 8);
    assert(!rejected_stream && !rejected_table && buffers.empty());
    assert(isError<err::device::VulkanCallFailed>(rejected_stream.error()));
    assert(isError<err::device::VulkanCallFailed>(rejected_table.error()));
    assert(rejected_stream.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
    assert(rejected_table.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
    assert(retirement.pendingCount() == 0);
    failure = EFailure::NONE;
    assert(!Stream::create(device, retirement, UINT32_MAX));
    assert(!MeshSectionTable::create(device, retirement, UINT32_MAX));
    assert(buffers.empty());
    {
        DeferredDestroyQueue other;
        other.init(device.vmaAllocator(), device.logicalDevice());
        retirement.beginFrame(9);
        other.beginFrame(12);
        auto source = Stream::create(device, retirement, 2);
        auto destination = Stream::create(device, other, 4);
        assert(source && destination);
        source->at(1) = 101;
        source->markDirty(1);
        const auto source_buffer = source->buffer();
        const auto replaced_buffer = destination->buffer();
        *destination = std::move(*source);
        assert(destination->buffer() == source_buffer && destination->at(1) == 101);
        assert(destination->capacity() == 2 && destination->hasDirtyPages());
        assert(other.pendingCount() == 1 && retirement.pendingCount() == 0);
        other.collect(11);
        assert(buffers.contains(replaced_buffer));
        other.collect(12);
        assert(!buffers.contains(replaced_buffer) && buffers.contains(source_buffer));
    }
    assert(retirement.pendingCount() == 1);
    retirement.collect(9);
    assert(buffers.empty());
    {
        auto cpu = MeshSectionTable::createCpu(0);
        assert(cpu.buffer() == VK_NULL_HANDLE && !cpu.hasWork());
        const MeshSectionRecord value{11, 12, 13, 14};
        assert(cpu.registerSection(value, 1, VK_INDEX_TYPE_UINT16) == 0);
        assert(cpu.registerSection(value, 1, VK_INDEX_TYPE_UINT16) == 0);
        assert(cpu.registerSection(value, 2, VK_INDEX_TYPE_UINT16) == 1);
        assert(cpu.registerSection(value, 1, VK_INDEX_TYPE_UINT32) == 2);
        cpu.unregisterSection(0);
        assert(cpu.at(0).first_index == 11);
        cpu.unregisterSection(0);
        assert(cpu.at(0).first_index == 0 && cpu.at(1).first_index == 11);
        assert(cpu.registerSection(value, 3) == 0);
        assert(cpu.ensureCapacity(4096) && !cpu.hasWork() && buffers.empty());
        auto moved = std::move(cpu);
        assert(moved.at(0).first_index == 11 && moved.at(2).index_count == 12);
    }
    retirement.beginFrame(13);
    {
        auto created = TPagedGpuStream<uint32_t>::create(device, retirement, 2);
        assert(created);
        auto stream = std::move(*created);
        assert(stream.capacity() == 2 && stream.cpuData().size() == 2);
        const auto original = stream.buffer();
        stream.at(0) = 17;
        stream.markDirty(0);
        failure = EFailure::BUFFER;
        const auto previous_rejections = rejections;
        assert(stream.reserve(8)); // within existing native allocation, still grows CPU records
        assert(rejections == previous_rejections && stream.buffer() == original);
        assert(stream.capacity() == 8 && stream.cpuData().size() == 8 && stream.at(0) == 17);
        stream.at(7) = 71;
        stream.markDirty(7);
        assert(!stream.reserve(65));
        assert(stream.capacity() == 8 && stream.buffer() == original && stream.at(7) == 71);
        assert(stream.hasDirtyPages());
        failure = EFailure::NONE;
        assert(stream.reserve(65) && stream.capacity() == 65 && stream.buffer() != original);
        assert(stream.at(0) == 17 && stream.at(7) == 71);
        assert(stream.reserve(80) && stream.capacity() == 80 && stream.cpuData().size() == 80);
        stream.at(79) = 79;
        stream.markDirty(79);
        assert(stream.reserve(1025));
        stream.at(512) = 512;
        stream.at(1024) = 1024;
        stream.markDirty(512);
        stream.markDirty(1024);
        stream.markDirty(1024);
        std::vector<TPagedGpuStream<uint32_t>::UploadChunk> chunks;
        assert(stream.collectUploadChunks(1025, false, chunks) == 1025 * sizeof(uint32_t));
        assert(
            chunks.size() == 1 && chunks[0].dst_offset == 0 &&
            chunks[0].src == reinterpret_cast<const uint8_t*>(stream.cpuData().data())
        );
        stream.clearDirtyState();
        assert(!stream.hasDirtyPages());
        chunks.clear();
        assert(stream.collectUploadChunks(1025, false, chunks) == 0 && chunks.empty());
        assert(stream.collectUploadChunks(1025, true, chunks) == 1025 * sizeof(uint32_t));
        std::vector<uint32_t> remap(1025, ~0u);
        remap[7] = 0;
        remap[1024] = 1;
        stream.compact(remap, 2);
        assert(stream.at(0) == 71 && stream.at(1) == 1024 && !stream.hasDirtyPages());
        const auto accepted_buffer = stream.buffer();
        assert(!stream.reserve(UINT32_MAX));
        assert(stream.capacity() == 1025 && stream.buffer() == accepted_buffer && stream.at(0) == 71);
        retirement.collect(12);
        assert(buffers.contains(original));
    }
    retirement.collect(13);
    assert(buffers.empty());
    {
        auto created = MeshSectionTable::create(device, retirement, 2);
        assert(created);
        auto table = std::move(*created);
        for (unsigned i = 0; i < 130; ++i)
        {
            MeshSectionRecord value{i * 3, 3, static_cast<int32_t>(i), i + 1};
            const auto id = table.registerSection(value, 0, VK_INDEX_TYPE_UINT32);
            assert(id == i && table.at(id).first_index == value.first_index);
            assert(table.registerSection(value, 0, VK_INDEX_TYPE_UINT32) == id);
            table.unregisterSection(id); // retain the first reference
            assert(table.at(id).vertex_count == i + 1);
        }
        TransferScheduler scheduler;
        assert(scheduler.init({device.vmaAllocator(), 16 * 1024, 1}));
        assert(table.hasWork());
        table.submitTransfers(scheduler);
        assert(scheduler.hasWork() && !table.hasWork());
        const auto original_buffer = table.buffer();
        failure = EFailure::BUFFER;
        assert(!table.ensureCapacity(4096));
        assert(table.buffer() == original_buffer && table.at(129).first_index == 387);
        failure = EFailure::NONE;
        assert(table.ensureCapacity(4096));
        assert(table.at(129).first_index == 387 && table.hasWork());
        table.submitTransfers(scheduler);
        assert(!table.hasWork());
        for (unsigned i = 0; i < 130; ++i)
        {
            table.unregisterSection(i);
        }
        retirement.collect(12);
        assert(buffers.contains(original_buffer));
    }
    retirement.collect(13);
    assert(buffers.empty() && retirement.pendingCount() == 0);
    std::puts("Paged/mesh complete backing: exact failure, CPU/native extent, cross-queue move, CPU-only variant, "
              "transfer re-upload and retirement PASS");
}

void checkSceneResources(
    lux::render::DeviceContext& device,
    const lux::render::GeneralDescriptorSetLayout& layouts,
    lux::render::DeferredDestroyQueue& retirement
)
{
    using namespace lux::render;
    static_assert(!std::is_default_constructible_v<SceneResources>);
    static_assert(!std::is_copy_constructible_v<SceneResources>);
    static_assert(!std::is_move_constructible_v<SceneResources>);
    static_assert(!std::is_default_constructible_v<SceneDomainDescriptorSets>);
    static_assert(!std::is_copy_constructible_v<SceneDomainDescriptorSets>);
    static_assert(!std::is_move_constructible_v<SceneDomainDescriptorSets>);
    const auto original_pools = pools.size();
    const auto original_sets = sets.size();
    SceneDescriptorArena::PoolSizeTemplate sizes;
    for (const auto domain : SceneDomainDescriptorSets::kPerSceneDomains)
    {
        const auto counts = domainDescriptorCounts(domain);
        sizes.storage_buffer += counts.storage_buffer * 2;
        sizes.combined_image_sampler += counts.combined_image_sampler * 2;
        sizes.uniform_buffer += counts.uniform_buffer * 2;
    }
    for (unsigned frames : {0u, kMaxFramesInFlight + 1u})
    {
        auto arena = SceneDescriptorArena::create(device.logicalDevice(), sizes);
        assert(arena);
        assert(!SceneDomainDescriptorSets::create(**arena, layouts, frames));
        assert((*arena)->poolCount() == 0);
    }
    // Reject each actual domain allocation, retain its successful prefix only in the arena.
    for (unsigned boundary = 0; boundary < 4; ++boundary)
    {
        auto arena = SceneDescriptorArena::create(device.logicalDevice(), sizes);
        failure = EFailure::SET;
        set_failure_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        skip_rejections = boundary;
        auto rejected = SceneDomainDescriptorSets::create(**arena, layouts, 2);
        assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
        assert(rejected.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        assert(sets.size() == original_sets + boundary);
        failure = EFailure::NONE;
        auto retry = SceneDomainDescriptorSets::create(**arena, layouts, 2);
        assert(retry);
        for (const auto domain : SceneDomainDescriptorSets::kPerSceneDomains)
        {
            assert((*retry)->setsFor(domain).size() == 2);
            assert((*retry)->set(domain, 0) && (*retry)->set(domain, 1));
            assert(!(*retry)->set(domain, 2));
        }
    }
    assert(pools.size() == original_pools && sets.size() == original_sets);
    {
        auto arena = SceneDescriptorArena::create(device.logicalDevice(), sizes);
        auto domains = SceneDomainDescriptorSets::create(**arena, layouts, 2);
        assert(domains);
        const auto targets = (*domains)->setsFor(lux::rdesc::EBindFrequency::GLOBAL);
        SceneResources::CreateInfo info{
            .device_context = device,
            .deferred_queue = retirement,
            .slices = 2,
            .initial_scene_capacity = 2,
            .initial_view_capacity = 2,
            .arena = arena->get(),
            .set_layout = layouts.getLayout(EDescriptorSetSlot::SCENE),
            .domain_sets = targets,
            .domain_binding_offset = engineSetDomainOffset(static_cast<uint32_t>(EDescriptorSetSlot::SCENE))
        };
        const std::array partial{targets[0], VkDescriptorSet{}};
        for (unsigned invalid = 0; invalid < 10; ++invalid)
        {
            auto bad = info;
            switch (invalid)
            {
            case 0:
                bad.slices = 0;
                break;
            case 1:
                bad.slices = kMaxFramesInFlight + 1;
                break;
            case 2:
                bad.initial_scene_capacity = 0;
                break;
            case 3:
                bad.initial_view_capacity = 0;
                break;
            case 4:
                bad.arena = nullptr;
                break;
            case 5:
                bad.set_layout = VK_NULL_HANDLE;
                break;
            case 6:
                bad.domain_sets = partial;
                break;
            case 7:
                bad.domain_sets = targets.first(1);
                break;
            case 8:
                bad.binding_view_data = bad.binding_scene_global;
                break;
            case 9:
                bad.domain_binding_offset = UINT32_MAX;
                break;
            }
            const auto writes = descriptor_writes;
            const auto set_count = sets.size();
            auto rejected = SceneResources::create(bad);
            assert(!rejected && isError<err::internal::InvalidArgument>(rejected.error()));
            assert(descriptor_writes == writes && sets.size() == set_count && buffers.empty());
        }
        retirement.beginFrame(11);
        for (auto boundary : {EFailure::BUFFER, EFailure::MAPPED, EFailure::SET})
        {
            for (unsigned prefix = 0; prefix < 2; ++prefix)
            {
                const auto writes = descriptor_writes;
                ResourceRegistry registry;
                failure = boundary;
                skip_rejections = prefix;
                set_failure_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
                auto rejected = SceneResources::create(info);
                assert(!rejected && !registry.find<SceneResources>() && descriptor_writes == writes);
                assert(isError<err::device::VulkanCallFailed>(rejected.error()));
                const auto native_error =
                    boundary == EFailure::MAPPED ? VK_ERROR_MEMORY_MAP_FAILED : VK_ERROR_OUT_OF_DEVICE_MEMORY;
                assert(rejected.error().args[0] == encodeVkResult(native_error));
                // Complete leaf buffers still use the original serial retirement queue.
                const auto retained = buffers.size();
                retirement.collect(10);
                assert(buffers.size() == retained);
                retirement.collect(11);
                assert(buffers.empty());
                failure = EFailure::NONE;
            }
        }
        set_failure_result = VK_ERROR_OUT_OF_POOL_MEMORY;
        trace_buffer_writes = true;
        buffer_writes.clear();
        auto candidate = SceneResources::create(info);
        assert(candidate && buffer_writes.size() == 8 && buffers.size() == 2);
        ResourceRegistry registry;
        auto published = registry.insert(std::move(*candidate));
        auto& scene = *published.get();
        const auto global = scene.allocateScene();
        const auto view = scene.allocateView();
        assert(global.isValid() && view.isValid());
        for (unsigned frame = 0; frame < 2; ++frame)
        {
            assert(scene.getDescriptorSet(frame) && sets.contains(scene.getDescriptorSet(frame)));
            scene.beginFrame(frame);
            scene.writeSceneGlobal(global, SceneGlobalGpuData{1, 2, frame, 3});
            ViewGpuData data{};
            data.viewport[0] = 640;
            scene.writeView(view, data);
            scene.writeViewData(view, &data, sizeof(data), frame);
        }
        assert(buffer_writes.size() == 8); // stable frame does not rewrite descriptors
        for (unsigned frame = 0; frame < 2; ++frame)
        {
            const auto& scene_write = buffer_writes[frame * 4];
            const auto& view_write = buffer_writes[frame * 4 + 1];
            assert(buffer_writes[frame * 4 + 2].set == targets[frame]);
            assert(buffer_writes[frame * 4 + 3].set == targets[frame]);
            VmaAllocationInfo scene_mapping{}, view_mapping{};
            const auto& scene_origin = buffers.at(scene_write.buffer);
            const auto& view_origin = buffers.at(view_write.buffer);
            vmaGetAllocationInfo(scene_origin.first, scene_origin.second, &scene_mapping);
            vmaGetAllocationInfo(view_origin.first, view_origin.second, &view_mapping);
            const auto* scene_data = reinterpret_cast<const SceneGlobalGpuData*>(
                static_cast<const std::byte*>(scene_mapping.pMappedData) + scene_write.offset
            );
            const auto* view_data = reinterpret_cast<const ViewGpuData*>(
                static_cast<const std::byte*>(view_mapping.pMappedData) + view_write.offset
            );
            assert(scene_data[global.index].frame_number == frame && scene_data[global.index].time_sec == 1);
            assert(view_data[view.index].viewport[0] == 640);
        }
        scene.freeScene(global);
        scene.freeView(view);
        const auto next_global = scene.allocateScene();
        const auto next_view = scene.allocateView();
        assert(next_global.index == global.index && next_global.gen != global.gen);
        assert(next_view.index == view.index && next_view.gen != view.gen);
        assert(scene.reserveScenes(128) && scene.reserveViews(128));
        buffer_writes.clear();
        scene.beginFrame(0);
        scene.beginFrame(0);
        scene.beginFrame(1);
        assert(buffer_writes.size() == 8);
        trace_buffer_writes = false;
        registry.shutdown();
        assert(retirement.pendingCount() != 0);
        retirement.collect(10);
        assert(!buffers.empty());
        retirement.collect(11);
        assert(buffers.empty());
        assert(sets.contains(targets[0]) && sets.contains(targets[1]));
    }
    assert(pools.size() == original_pools && sets.size() == original_sets && buffers.empty());
    std::puts(
        "Scene backing: complete domain targets, exact failures, retry, publication, revisions and retirement PASS"
    );
}

void checkDescriptorRegistration(lux::render::DeviceContext& device)
{
    using namespace lux::render;
    const auto initial_live = layout_creations - layout_destroys;
    {
        DescriptorService service(device.logicalDevice());
        const auto first = service.registerLayout(storageBufferVertexLayout("first"));
        assert(first && *first == 0);
        const auto first_handle = service.layout(*first);
        assert(first_handle);
        const VkDescriptorSetLayoutBinding
            next_binding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        const DescriptorLayoutDesc second{{&next_binding, 1}, {}, 0, "second"};
        const auto created_before = layout_creations;
        const auto rejected_before = rejections;
        failure = EFailure::LAYOUT;
        const auto rejected = service.registerLayout(second);
        failure = EFailure::NONE;
        assert(!rejected && rejections == rejected_before + 1);
        assert(isError<err::device::VulkanCallFailed>(rejected.error()));
        assert(rejected.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        assert(layout_creations == created_before && service.layout(*first) == first_handle);
        assert(!service.layout(1));
        const auto retry = service.registerLayout(second);
        assert(retry && *retry == 1 && service.layout(*retry));
        const auto stable_creations = layout_creations;
        for (unsigned i = 0; i < 30; ++i)
        {
            const auto first_again = service.registerLayout(storageBufferVertexLayout("alias label"));
            const auto second_again = service.registerLayout(second);
            assert(first_again && *first_again == *first);
            assert(second_again && *second_again == *retry);
        }
        assert(layout_creations == stable_creations);
        const std::array<VkDescriptorBindingFlags, 2> invalid_flags{};
        auto invalid = second;
        invalid.binding_flags = invalid_flags;
        const auto invalid_result = service.registerLayout(invalid);
        assert(!invalid_result && isError<err::internal::InvalidArgument>(invalid_result.error()));
        assert(layout_creations == stable_creations && service.layout(*first) == first_handle);
    }
    assert(layout_creations - layout_destroys == initial_live);
    std::puts("Descriptor registration: exact native rejection, no ID publication, retry, cache identity and final "
              "release PASS");
}

void checkMdcPublication(
    lux::render::DeviceContext& device,
    lux::render::GeneralDescriptorSetLayout& layouts,
    lux::render::DeferredDestroyQueue& retirement
)
{
    using namespace lux::render;
    retirement.flushAll();
    assert(buffers.empty());
    auto arena = SceneDescriptorArena::create(device.logicalDevice(), {});
    assert(arena);
    const auto target = (*arena)->allocate(layouts.getLayout(EDescriptorSetSlot::INSTANCE));
    assert(target);
    const std::array targets{*target};
    retirement.beginFrame(101);
    {
        auto complete = InstanceResources::create({device, retirement, targets});
        assert(complete);
        auto& instances = **complete;
        FrameStamp stamp{};
        std::array<VkBuffer, kMaxFramesInFlight + 1> accepted{};
        std::array<std::vector<uint32_t>, kMaxFramesInFlight + 1> content{};
        const auto verify_contents = [&]
        {
            for (unsigned slot = 0; slot < accepted.size(); ++slot)
            {
                assert(instances.mdcInfoBufferAt(slot) == accepted[slot]);
                if (!accepted[slot])
                {
                    continue;
                }
                const auto& allocation = buffers.at(accepted[slot]);
                VmaAllocationInfo info{};
                vmaGetAllocationInfo(allocation.first, allocation.second, &info);
                assert(info.pMappedData);
                assert(
                    std::memcmp(info.pMappedData, content[slot].data(), content[slot].size() * sizeof(uint32_t)) == 0
                );
            }
        };
        const auto accept = [&]
        {
            assert(instances.uploadMdcInfo());
            const auto slot = instances.currentMdcInfoSlot();
            accepted[slot] = instances.mdcInfoBuffer();
            content[slot] = instances.mdcTable().gpuData();
            verify_contents();
        };
        // First acquisition and replacement both use the same native admission boundaries.
        const auto reject_upload = [&](EFailure boundary)
        {
            const auto buffer = instances.mdcInfoBuffer();
            const auto slot = instances.currentMdcInfoSlot();
            const auto count = buffers.size();
            const auto pending = retirement.pendingCount();
            const auto serial = instances.mdcTable().layoutSerial();
            const auto rejected_before = rejections;
            failure = boundary;
            const auto rejected = instances.uploadMdcInfo();
            failure = EFailure::NONE;
            assert(!rejected && rejections == rejected_before + 1);
            assert(isError<err::device::VulkanCallFailed>(rejected.error()));
            const auto expected =
                boundary == EFailure::BUFFER ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_MEMORY_MAP_FAILED;
            assert(rejected.error().args[0] == encodeVkResult(expected));
            assert(instances.mdcInfoBuffer() == buffer && instances.currentMdcInfoSlot() == slot);
            assert(buffers.size() == count && retirement.pendingCount() == pending);
            assert(instances.mdcTable().layoutSerial() == serial);
            verify_contents();
        };
        for (auto boundary : {EFailure::BUFFER, EFailure::MAPPED, EFailure::FLUSH})
        {
            reject_upload(boundary);
        }
        accept();
        for (unsigned i = 1; i < accepted.size(); ++i)
        {
            instances.mdcTable().registerInstance(0, 0, i, 0, VK_INDEX_TYPE_UINT32);
            stamp.serial = i;
            instances.onFrameBeginMaintenance(stamp);
            accept();
        }
        const auto replaced_slot = (instances.currentMdcInfoSlot() + 1u) % accepted.size();
        const auto old = accepted[replaced_slot];
        assert(old && retirement.pendingCount() == 0);
        instances.mdcTable().registerInstance(0, 0, 100, 0, VK_INDEX_TYPE_UINT32);
        ++stamp.serial;
        instances.onFrameBeginMaintenance(stamp);
        for (auto boundary : {EFailure::BUFFER, EFailure::MAPPED, EFailure::FLUSH})
        {
            reject_upload(boundary);
        }
        accept();
        assert(instances.currentMdcInfoSlot() == replaced_slot && instances.mdcInfoBuffer() != old);
        assert(retirement.pendingCount() == 1 && buffers.contains(old));
        retirement.collect(100);
        assert(buffers.contains(old));
        retirement.collect(101);
        assert(!buffers.contains(old));
        retirement.beginFrame(102);

        // Multiple features and later unrelated graph compiles share immutable accepted data.
        const auto stable = instances.mdcInfoBuffer();
        measure_acquisitions = true;
        acquisition = 0;
        fail_acquisition = 0;
        for (unsigned i = 0; i < 30; ++i)
        {
            stamp.serial++;
            instances.onFrameBeginMaintenance(stamp);
            assert(instances.uploadMdcInfo());
            assert(instances.uploadMdcInfo());
            assert(instances.mdcInfoBuffer() == stable);
        }
        measure_acquisitions = false;
        assert(acquisition == 0 && retirement.pendingCount() == 0);
        // Same-sized changed offsets still require a complete candidate, never overwrite old bytes.
        instances.mdcTable().registerInstance(0, 0, 100, 0, VK_INDEX_TYPE_UINT32);
        reject_upload(EFailure::FLUSH);
        accept();
        assert(instances.mdcInfoBuffer() != stable);
    }
    const auto pending = buffers.size();
    assert(pending > 0 && retirement.pendingCount() == pending);
    retirement.collect(101);
    assert(buffers.size() == pending);
    retirement.collect(102);
    assert(buffers.empty());
    std::puts("MDC publication: allocation/map/flush rejection, immutable captured slots, retry, zero stable "
              "acquisitions and serial retirement PASS");
}

void checkInstanceConstruction(
    lux::render::DeviceContext& device,
    lux::render::GeneralDescriptorSetLayout& layouts,
    lux::render::DeferredDestroyQueue& retirement
)
{
    using namespace lux::render;
    static_assert(!std::is_default_constructible_v<InstanceResources>);
    static_assert(!std::is_copy_constructible_v<InstanceResources>);
    static_assert(!std::is_move_constructible_v<InstanceResources>);
    static_assert(!std::is_default_constructible_v<InstanceSlotRegistry>);
    retirement.flushAll();
    assert(buffers.empty());
    auto arena = SceneDescriptorArena::create(device.logicalDevice(), {});
    assert(arena);
    std::array<VkDescriptorSet, 2> targets{};
    for (auto& target : targets)
    {
        const auto allocated = (*arena)->allocate(layouts.getLayout(EDescriptorSetSlot::INSTANCE));
        assert(allocated);
        target = *allocated;
    }
    InstanceResources::CreateInfo info{device, retirement, targets};
    const auto check_invalid = [&]
    {
        const auto writes = descriptor_writes;
        auto rejected = InstanceResources::create(info);
        assert(!rejected && isError<err::internal::InvalidArgument>(rejected.error()));
        assert(buffers.empty() && retirement.pendingCount() == 0 && descriptor_writes == writes);
    };
    info.domain_sets = {};
    check_invalid();
    const std::array incomplete{targets[0], VkDescriptorSet{}};
    info.domain_sets = incomplete;
    check_invalid();
    info.domain_sets = targets;
    info.domain_binding_offset = UINT32_MAX;
    check_invalid();
    info.domain_binding_offset = 0;
    info.initial_capacity = 0;
    check_invalid();
    info.initial_capacity = info.max_capacity + 1;
    check_invalid();
    info.initial_capacity = 1;
    info.max_capacity = UINT32_MAX;
    check_invalid();
    info.max_capacity = 3 * kInstanceSlotsPerPage;
    for (const double size :
         {0.0,
          -1.0,
          std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::quiet_NaN(),
          std::numeric_limits<double>::denorm_min()})
    {
        info.coordinate_page_size = size;
        check_invalid();
    }
    info.coordinate_page_size = 1024;
    info.initial_capacity = 2 * kInstanceSlotsPerPage + 1;
    for (const bool sparse : {false, true})
    {
        info.sparse_bda = sparse;
        unsigned acquisitions{};
        {
            acquisition = 0;
            fail_acquisition = 0;
            measure_acquisitions = true;
            auto complete = InstanceResources::create(info);
            measure_acquisitions = false;
            assert(complete);
            acquisitions = acquisition;
        }
        retirement.flushAll();
        assert(buffers.empty());
        for (unsigned boundary = 1; boundary <= acquisitions; ++boundary)
        {
            ResourceRegistry registry;
            const auto writes = descriptor_writes;
            const auto prior_rejections = rejections;
            acquisition = 0;
            fail_acquisition = boundary;
            measure_acquisitions = true;
            auto rejected = InstanceResources::create(info);
            measure_acquisitions = false;
            assert(!rejected && rejections == prior_rejections + 1);
            assert(registry.find<InstanceResources>() == nullptr && descriptor_writes == writes);
            if (rejected_boundary == EFailure::ADDRESS)
            {
                assert(isError<err::memory::BufferDeviceAddressUnavailable>(rejected.error()));
            }
            else
            {
                const auto vk_error =
                    rejected_boundary == EFailure::BUFFER ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_MEMORY_MAP_FAILED;
                assert(isError<err::device::VulkanCallFailed>(rejected.error()));
                assert(rejected.error().args[0] == encodeVkResult(vk_error));
            }
            // Complete leaf owners preserve the existing serial-retirement policy even before registry adoption.
            assert(retirement.pendingCount() == buffers.size());
            retirement.flushAll();
            assert(buffers.empty());
        }
        retirement.beginFrame(91);
        {
            trace_buffer_writes = true;
            buffer_writes.clear();
            auto complete = InstanceResources::create(info);
            assert(complete && buffer_writes.size() == 4);
            auto* instances = complete->get();
            assert(instances->capacity() == info.initial_capacity && instances->maximumCapacity() == info.max_capacity);
            assert(instances->residentPageCount() == 3 && instances->slotCount() == 0);
            assert(instances->descriptorWriteCount() == 4);
            if (sparse)
            {
                assert(instances->pageTableLeafCount() == 1);
                const auto root = buffer_writes[0].buffer;
                const auto& root_allocation = buffers.at(root);
                VmaAllocationInfo root_info{};
                vmaGetAllocationInfo(root_allocation.first, root_allocation.second, &root_info);
                assert(root_info.pMappedData && static_cast<VkDeviceAddress*>(root_info.pMappedData)[0] != 0);
                unsigned leaves{};
                for (const auto& [buffer, allocation] : buffers)
                {
                    VmaAllocationInfo mapped{};
                    vmaGetAllocationInfo(allocation.first, allocation.second, &mapped);
                    if (buffer == root || !mapped.pMappedData)
                    {
                        continue;
                    }
                    ++leaves;
                    const auto* entries = static_cast<const GpuInstancePageAddresses*>(mapped.pMappedData);
                    for (unsigned page = 0; page < 3; ++page)
                    {
                        assert(entries[page].transform && entries[page].previous_transform);
                        assert(entries[page].property && entries[page].cull_meta);
                        if (page > 0)
                        {
                            assert(entries[page].transform != entries[page - 1].transform);
                        }
                    }
                    assert(entries[3].transform == 0);
                }
                assert(leaves == 1);
            }
            else
            {
                assert(buffer_writes[0].buffer == instances->transformBuffer());
            }
            for (unsigned slice = 0; slice < targets.size(); ++slice)
            {
                assert(buffer_writes[slice * 2].set == targets[slice]);
                assert(buffer_writes[slice * 2].binding == 0 && buffer_writes[slice * 2 + 1].binding == 1);
            }
            ResourceRegistry registry;
            const auto inserted = registry.insert(std::move(*complete));
            assert(inserted.get() == instances && registry.find<InstanceResources>() == instances);
            const auto object = instances->allocateObject();
            assert(object && instances->isAlive(object));
            FrameStamp stamp{};
            stamp.serial = 91;
            instances->onFrameBeginMaintenance(stamp);
            assert(instances->uploadMdcInfo());
            assert(instances->mdcInfoBuffer());
            assert(buffers.contains(instances->mdcInfoBufferAt(instances->currentMdcInfoSlot())));
            trace_buffer_writes = false;
            buffer_writes.clear();
        }
        const auto pending = buffers.size();
        assert(pending > 0 && pending == retirement.pendingCount());
        retirement.collect(90);
        assert(buffers.size() == pending);
        retirement.collect(91);
        assert(buffers.empty());
        std::printf("Instance mandatory construction: sparse=%d native boundaries=%u PASS\n", sparse, acquisitions);
    }
}

void checkInstanceGrowth(
    lux::render::DeviceContext& device,
    lux::render::GeneralDescriptorSetLayout& layouts,
    lux::render::DeferredDestroyQueue& retirement
)
{
    using namespace lux::render;
    retirement.flushAll();
    assert(buffers.empty());
    auto arena = SceneDescriptorArena::create(device.logicalDevice(), {});
    assert(arena);
    std::array<VkDescriptorSet, 2> targets{};
    for (auto& target : targets)
    {
        auto allocated = (*arena)->allocate(layouts.getLayout(EDescriptorSetSlot::INSTANCE));
        assert(allocated);
        target = *allocated;
    }
    for (const bool sparse : {false, true})
    {
        retirement.beginFrame(83);
        {
            InstanceResources::CreateInfo info{device, retirement, targets};
            info.initial_capacity = kInstanceSlotsPerPage;
            info.max_capacity = 2 * kInstanceSlotsPerPage;
            info.sparse_bda = sparse;
            trace_buffer_writes = true;
            buffer_writes.clear();
            auto created = InstanceResources::create(info);
            assert(created);
            auto& instances = **created;
            assert(buffer_writes.size() == 4);
            const auto original_descriptor_buffer = buffer_writes[0].buffer;
            RenderObjectHandle first;
            for (std::uint32_t index = 0; index < info.initial_capacity; ++index)
            {
                const auto object = instances.allocateObject();
                assert(object.index == index);
                if (index == 0)
                {
                    first = object;
                }
            }
            const auto slot = instances.resolveSlot(first);
            instances.propertyAt(slot).rgba8 = 0x1a2b3c4du;
            instances.markPropertyDirty(slot);
            {
                TransferScheduler scheduler;
                assert(scheduler.init({device.vmaAllocator(), 8 * 1024 * 1024, 1}));
                instances.submitTransfers(scheduler);
                assert(scheduler.hasWork() && !instances.needsFullRebuild());
            }
            instances.propertyAt(slot).rgba8 = 0x5a6b7c8du;
            instances.markPropertyDirty(slot);
            const auto* stable_cpu = &instances.propertyAt(slot);
            const auto old_buffers = std::array{
                instances.transformBuffer(),
                instances.prevTransformBuffer(),
                instances.propertyBuffer(),
                instances.cullMetaBuffer()
            };
            const auto accepted_buffers = buffers;
            const auto original_writes = descriptor_writes;
            const auto original_leaves = instances.pageTableLeafCount();
            std::map<VkBuffer, std::vector<std::byte>> mapped_bytes;
            for (const auto& [buffer, allocation] : buffers)
            {
                VmaAllocationInfo mapped{};
                vmaGetAllocationInfo(allocation.first, allocation.second, &mapped);
                if (mapped.pMappedData)
                {
                    auto* data = static_cast<const std::byte*>(mapped.pMappedData);
                    mapped_bytes.emplace(buffer, std::vector<std::byte>(data, data + mapped.size));
                }
            }
            const auto assert_unchanged = [&]
            {
                assert(instances.capacity() == info.initial_capacity);
                assert(
                    instances.slotCount() == info.initial_capacity && instances.aliveCount() == info.initial_capacity
                );
                assert(instances.residentPageCount() == 1 && instances.pageTableLeafCount() == original_leaves);
                assert(instances.isAlive(first) && instances.handleForSlot(slot) == first);
                assert(&instances.propertyAt(slot) == stable_cpu && stable_cpu->rgba8 == 0x5a6b7c8du);
                assert(instances.transformBuffer() == old_buffers[0]);
                assert(instances.prevTransformBuffer() == old_buffers[1]);
                assert(instances.propertyBuffer() == old_buffers[2]);
                assert(instances.cullMetaBuffer() == old_buffers[3]);
                assert(!instances.needsFullRebuild() && instances.slotLayoutSerial() == 1);
                assert(buffers == accepted_buffers && retirement.pendingCount() == 0);
                assert(descriptor_writes == original_writes && buffer_writes.size() == 4);
                for (const auto& [buffer, bytes] : mapped_bytes)
                {
                    const auto& allocation = buffers.at(buffer);
                    VmaAllocationInfo mapped{};
                    vmaGetAllocationInfo(allocation.first, allocation.second, &mapped);
                    assert(std::memcmp(bytes.data(), mapped.pMappedData, bytes.size()) == 0);
                }
            };
            for (auto boundary : {EFailure::BUFFER, EFailure::ADDRESS, EFailure::FLUSH})
            {
                if (!sparse && boundary != EFailure::BUFFER)
                {
                    continue;
                }
                const unsigned attempts = boundary == EFailure::FLUSH ? 1 : 4;
                for (unsigned index = 0; index < attempts; ++index)
                {
                    failure = boundary;
                    skip_rejections = index;
                    const auto before_rejections = rejections;
                    assert(!instances.allocateObject());
                    assert(rejections > before_rejections);
                    failure = EFailure::NONE;
                    assert_unchanged();
                }
            }
            // Dirty bytes must survive all rejected candidates and still reach the original upload path.
            {
                TransferScheduler scheduler;
                assert(scheduler.init({device.vmaAllocator(), 8 * 1024 * 1024, 1}));
                instances.submitTransfers(scheduler);
                assert(scheduler.hasWork() && !instances.needsFullRebuild());
            }
            const auto grown = instances.allocateObject();
            assert(grown && grown.index == info.initial_capacity);
            assert(instances.capacity() == info.max_capacity && instances.residentPageCount() == 2);
            assert(instances.isAlive(first) && instances.handleForSlot(slot) == first);
            assert(&instances.propertyAt(slot) == stable_cpu && stable_cpu->rgba8 == 0x5a6b7c8du);
            assert(instances.slotLayoutSerial() == 1);
            if (sparse)
            {
                assert(retirement.pendingCount() == 0 && buffers.size() == accepted_buffers.size() + 4);
                assert(buffer_writes.size() == 4 && !instances.needsFullRebuild());
                assert(buffer_writes[0].buffer == original_descriptor_buffer);
            }
            else
            {
                assert(retirement.pendingCount() == 4 && buffers.size() == accepted_buffers.size() + 4);
                assert(instances.needsFullRebuild() && buffer_writes.size() == 8);
                for (unsigned slice = 0; slice < targets.size(); ++slice)
                {
                    assert(buffer_writes[4 + slice * 2].set == targets[slice]);
                    assert(buffer_writes[4 + slice * 2].buffer == instances.transformBuffer());
                    assert(buffer_writes[5 + slice * 2].buffer == instances.propertyBuffer());
                }
                for (const auto buffer : old_buffers)
                {
                    assert(buffers.contains(buffer));
                }
            }
            instances.freeObject(first);
            const auto reused = instances.allocateObject();
            assert(reused.index == first.index && reused.gen != first.gen && !instances.isAlive(first));
            trace_buffer_writes = false;
            buffer_writes.clear();
        }
        const auto retained = buffers.size();
        assert(retained > 0 && retirement.pendingCount() == retained);
        retirement.collect(82);
        assert(buffers.size() == retained);
        retirement.collect(83);
        assert(buffers.empty() && retirement.pendingCount() == 0);
    }
    std::puts("Instance aggregate: all field failures preserve accepted buffers, descriptors, dirty bytes, mapped "
              "page table and identities; successful retry commits together, original retirement PASS");
}

void checkCanvasConstruction(
    lux::render::DeviceContext& device,
    lux::render::ResourceContext& resources,
    lux::render::GeneralDescriptorSetLayout& layouts,
    lux::render::DeferredDestroyQueue& retirement
)
{
    using namespace lux::render;
    static_assert(!std::is_default_constructible_v<Canvas2DInstanceArena>);
    static_assert(!std::is_copy_constructible_v<Canvas2DInstanceArena>);
    static_assert(!std::is_move_constructible_v<Canvas2DInstanceArena>);
    const auto initial_buffers = buffers.size();
    const auto initial_sets = sets.size();
    {
        TextureResources::CreateInfo texture_info{};
        texture_info.combined_ci.resource_context = &resources;
        texture_info.combined_ci.deferred_queue = &retirement;
        texture_info.combined_ci.descriptor_set_layout = layouts.getLayout(TGetBindingSet<ETextureSetBindings>::value);
        texture_info.combined_ci.layout_max_capacity = layouts.bindless2DCount();
        texture_info.combined_ci.initial_capacity = 8;
        texture_info.cube_max_capacity = layouts.bindlessCubeCount();
        auto textures = TextureResources::create(texture_info);
        assert(textures);
        DescriptorService descriptors(device.logicalDevice());
        auto arena = SceneDescriptorArena::create(device.logicalDevice(), {});
        assert(arena);
        const auto texture_buffers = buffers.size();
        const auto texture_sets = sets.size();
        const Canvas2DInstanceArena::CreateInfo info{device, retirement, **textures, descriptors, **arena, 256, 512, 2};
        retirement.beginFrame(111);
        for (unsigned invalid = 0; invalid < 4; ++invalid)
        {
            auto bad = info;
            switch (invalid)
            {
            case 0:
                bad.initial_capacity = 0;
                break;
            case 1:
                bad.max_capacity = 0;
                break;
            case 2:
                bad.initial_capacity = bad.max_capacity + 1;
                break;
            case 3:
                bad.max_capacity = UINT32_MAX;
                break;
            }
            const auto native_layouts = layout_creations;
            auto rejected = Canvas2DInstanceArena::create(bad);
            assert(!rejected && isError<err::internal::InvalidArgument>(rejected.error()));
            assert(layout_creations == native_layouts && buffers.size() == texture_buffers);
        }
        for (const auto boundary : {EFailure::LAYOUT, EFailure::BUFFER, EFailure::POOL, EFailure::SET})
        {
            const unsigned attempts = boundary == EFailure::BUFFER ? 6 : 1;
            for (unsigned prefix = 0; prefix < attempts; ++prefix)
            {
                const auto writes = descriptor_writes;
                const auto previous_rejections = rejections;
                ResourceRegistry registry;
                failure = boundary;
                skip_rejections = prefix;
                set_failure_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
                auto rejected = Canvas2DInstanceArena::create(info);
                failure = EFailure::NONE;
                assert(!rejected && rejections == previous_rejections + 1);
                assert(isError<err::device::VulkanCallFailed>(rejected.error()));
                assert(rejected.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
                assert(!registry.find<Canvas2DInstanceArena>() && descriptor_writes == writes);
                assert(sets.size() == texture_sets && (*arena)->poolCount() == 0);
                retirement.collect(110);
                retirement.collect(111);
                assert(buffers.size() == texture_buffers);
            }
        }
        trace_buffer_writes = true;
        buffer_writes.clear();
        ResourceRegistry registry;
        auto complete = Canvas2DInstanceArena::create(info);
        assert(complete && !registry.find<Canvas2DInstanceArena>());
        auto& canvas = *registry.insert(std::move(*complete));
        assert(registry.find<Canvas2DInstanceArena>() == &canvas);
        assert(buffer_writes.size() == 6 && sets.size() == texture_sets + 3);
        const auto initial_writes = buffer_writes;
        const auto image_set = canvas.descriptorSet(ECanvas2DKind::IMAGE);
        const auto field_set = canvas.descriptorSet(ECanvas2DKind::PIXEL_FIELD);
        const auto tile_set = canvas.descriptorSet(ECanvas2DKind::TILE);
        assert(image_set && field_set && tile_set);
        assert(image_set != field_set && field_set != tile_set && image_set != tile_set);
        assert(sets.at(image_set) == sets.at(field_set) && sets.at(image_set) == sets.at(tile_set));

        Image2DHandle image, first_image;
        PixelFieldInstanceHandle field, first_field;
        Tile2DInstanceHandle tile, first_tile;
        PixelField2DInstanceData field_data{};
        field_data.m[4] = 42.0f;
        Tile2DInstanceData tile_data{};
        tile_data.m[4] = 73.0f;
        Image2DInstanceData image_data{};
        image_data.tint = 0x12345678u;
        for (unsigned i = 0; i < 256; ++i)
        {
            assert(canvas.add(image_data, float(i), true, image, 1) == ECanvas2DCreateStatus::OK);
            if (i == 0)
            {
                first_image = image;
            }
        }
        for (unsigned i = 0; i < 64; ++i)
        {
            assert(canvas.addField(field_data, float(i), true, field) == ECanvas2DCreateStatus::OK);
            assert(canvas.addTile(tile_data, float(i), true, tile) == ECanvas2DCreateStatus::OK);
            if (i == 0)
            {
                first_field = field;
                first_tile = tile;
            }
        }
        const auto add_kind = [&](unsigned kind)
        {
            switch (kind)
            {
            case 0:
                return canvas.add(image_data, 0.5f, true, image, 1);
            case 1:
                return canvas.addField(field_data, 0.5f, true, field);
            default:
                return canvas.addTile(tile_data, 0.5f, true, tile);
            }
        };
        for (unsigned kind = 0; kind < 3; ++kind)
        {
            const auto old_record = initial_writes[kind * 2].buffer;
            const auto old_order = initial_writes[kind * 2 + 1].buffer;
            const auto writes = descriptor_writes;
            const auto live = canvas.liveCount();
            const auto rebuilds = canvas.orderRebuilds();
            for (unsigned prefix = 0; prefix < 2; ++prefix)
            {
                failure = EFailure::BUFFER;
                skip_rejections = prefix;
                assert(add_kind(kind) == ECanvas2DCreateStatus::CAPACITY_EXHAUSTED);
                failure = EFailure::NONE;
                assert(descriptor_writes == writes && canvas.liveCount() == live);
                assert(canvas.orderRebuilds() == rebuilds);
                // Only unpublished candidates can retire; accepted descriptors must still reference live backing.
                retirement.collect(111);
                assert(buffers.contains(old_record) && buffers.contains(old_order));
                assert(canvas.descriptorSet(ECanvas2DKind::IMAGE) == image_set);
                assert(canvas.descriptorSet(ECanvas2DKind::PIXEL_FIELD) == field_set);
                assert(canvas.descriptorSet(ECanvas2DKind::TILE) == tile_set);
            }
            assert(add_kind(kind) == ECanvas2DCreateStatus::OK);
            assert(canvas.liveCount() == live + 1 && descriptor_writes == writes + 2);
            assert(buffer_writes[buffer_writes.size() - 2].buffer != old_record);
            assert(buffer_writes.back().buffer != old_order);
            assert(buffers.contains(old_record) && buffers.contains(old_order));
            retirement.collect(110);
            assert(buffers.contains(old_record) && buffers.contains(old_order));
            retirement.collect(111);
            assert(!buffers.contains(old_record) && !buffers.contains(old_order));
        }
        // Previously accepted identities survive replacement, and removal/reuse keeps the original generation rule.
        canvas.remove(first_image);
        assert(canvas.add(image_data, 0.0f, true, image, 1) == ECanvas2DCreateStatus::OK);
        assert(image.index == first_image.index && image.gen != first_image.gen);
        canvas.removeField(first_field);
        assert(canvas.addField(field_data, 0.0f, true, field) == ECanvas2DCreateStatus::OK);
        assert(field.index == first_field.index && field.gen != first_field.gen);
        canvas.removeTile(first_tile);
        assert(canvas.addTile(tile_data, 0.0f, true, tile) == ECanvas2DCreateStatus::OK);
        assert(tile.index == first_tile.index && tile.gen != first_tile.gen);
        {
            TransferScheduler scheduler;
            assert(scheduler.init({device.vmaAllocator(), 256 * 1024, 1}));
            canvas.submitTransfers(scheduler);
            assert(scheduler.hasWork() && canvas.orderRebuilds() == 1);
            // Execute the real upload and read back the accepted records/order after all rejected candidates.
            VkBufferCreateInfo readback_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            readback_info.size = 240;
            readback_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo allocation_info{};
            allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
            allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
            auto readback = VmaBuffer::create(device.vmaAllocator(), readback_info, allocation_info);
            assert(readback);
            auto command = CommandBufferOwner::create(device.logicalDevice(), resources.commandPool());
            assert(command);
            VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            assert(begin(command->get(), &begin_info) == VK_SUCCESS);
            scheduler.beginTransfers(command->get());
            scheduler.recordCopies(command->get());
            scheduler.endTransfers(command->get());
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(command->get(), &dependency);
            for (unsigned kind = 0; kind < 3; ++kind)
            {
                const auto set = canvas.descriptorSet(static_cast<ECanvas2DKind>(kind));
                std::array<VkBuffer, 2> current{};
                for (const auto& write : buffer_writes)
                {
                    if (write.set == set)
                    {
                        current[write.binding] = write.buffer;
                    }
                }
                assert(current[0] && current[1]);
                const VkDeviceSize stride = kind == 0 ? 56 : 64;
                const VkBufferCopy record_copy{stride, kind * 80u, stride};
                const VkBufferCopy order_copy{0, kind * 80u + 64, 12};
                vkCmdCopyBuffer(command->get(), current[0], readback->buffer(), 1, &record_copy);
                vkCmdCopyBuffer(command->get(), current[1], readback->buffer(), 1, &order_copy);
            }
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            vkCmdPipelineBarrier2(command->get(), &dependency);
            assert(end(command->get()) == VK_SUCCESS);
            VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            auto fence = FenceOwner::create(device.logicalDevice(), fence_info);
            assert(fence);
            const auto cmd = command->get();
            VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submission.commandBufferCount = 1;
            submission.pCommandBuffers = &cmd;
            assert(submit(device.graphicsQueue(), 1, &submission, fence->get()) == VK_SUCCESS);
            const auto fence_handle = fence->get();
            assert(wait(device.logicalDevice(), 1, &fence_handle, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
            const auto* mapped = static_cast<const std::byte*>(readback->map());
            assert(mapped);
            assert(
                vmaInvalidateAllocation(device.vmaAllocator(), readback->allocation(), 0, VK_WHOLE_SIZE) == VK_SUCCESS
            );
            for (unsigned kind = 0; kind < 3; ++kind)
            {
                float transform[6]{};
                std::memcpy(transform, mapped + kind * 80, sizeof(transform));
                assert(transform[0] == 1.0f && transform[3] == 1.0f);
                assert(transform[4] == (kind == 0 ? 0.0f : (kind == 1 ? 42.0f : 73.0f)));
                std::array<std::uint32_t, 3> order{};
                std::memcpy(order.data(), mapped + kind * 80 + 64, sizeof(order));
                assert(order[0] == 0 && order[1] == (kind == 0 ? 256u : 64u) && order[2] == 1);
            }
            std::uint32_t tint{};
            std::memcpy(&tint, mapped + 48, sizeof(tint));
            assert(tint == image_data.tint);
            readback->unmap();
            unsigned drawn = 0;
            for (const auto& run : canvas.runs())
            {
                drawn += run.count;
            }
            assert(drawn == canvas.liveCount());
            const auto rebuilds = canvas.orderRebuilds();
            scheduler.resetFrame(0);
            for (unsigned i = 0; i < 30; ++i)
            {
                canvas.submitTransfers(scheduler);
            }
            assert(!scheduler.hasWork() && canvas.orderRebuilds() == rebuilds);
            canvas.setEnabled(false);
            assert(canvas.runs().empty());
            canvas.setEnabled(true);
            assert(!canvas.runs().empty());
        }
        trace_buffer_writes = false;
        buffer_writes.clear();
        set_failure_result = VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    retirement.collect(110);
    assert(buffers.size() > initial_buffers);
    retirement.collect(111);
    assert(buffers.size() == initial_buffers && sets.size() == initial_sets);
    std::puts("Canvas complete backing: six buffer boundaries, atomic descriptor family, three-kind rejected growth, "
              "accepted identity, retry, dirty uploads, ordering and original retirement PASS");
}

void checkShadowRebuild(
    lux::render::DeviceContext& device,
    lux::render::GeneralDescriptorSetLayout& layouts,
    lux::render::DeferredDestroyQueue& retirement
)
{
    using namespace lux::render;
    DescriptorService descriptors(device.logicalDevice());
    auto arena = SceneDescriptorArena::create(device.logicalDevice(), {});
    assert(arena);
    const std::array light_layouts{
        layouts.getLayout(TGetBindingSet<ELightSetBindings>::value),
        layouts.getLayout(TGetBindingSet<ELightSetBindings>::value)
    };
    auto light_sets = (*arena)->allocateBatch(light_layouts);
    assert(light_sets);
    LightResources::CreateInfo light_info{};
    light_info.ssbo_config = SSBOInitConfig{
        .device_context = &device,
        .deferred_queue = &retirement,
        .initial_dense_capacity = 16,
        .slices = 2
    };
    light_info.descriptor_svc = &descriptors;
    light_info.domain_sets = *light_sets;
    auto light = LightResources::create(light_info);
    assert(light);
    const std::array bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL_GRAPHICS},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS}
    };
    auto layout = descriptors.registerLayout({.bindings = bindings});
    assert(layout);
    {
        static_assert(!std::is_default_constructible_v<ShadowResources>);
        static_assert(!std::is_copy_constructible_v<ShadowResources>);
        static_assert(!std::is_move_constructible_v<ShadowResources>);
        const ShadowResources::CreateInfo info{device, descriptors, **arena, *light_sets, 0, *layout, 2, 16, 1, 4};
        const auto base_buffers = buffers.size();
        const auto base_images = images.size();
        const auto base_views = views.size();
        const auto base_sets = sets.size();
        for (unsigned invalid = 0; invalid < 9; ++invalid)
        {
            auto bad = info;
            auto invalid_sets = *light_sets;
            switch (invalid)
            {
            case 0:
                bad.frames_in_flight = 0;
                break;
            case 1:
                bad.frames_in_flight = kMaxFramesInFlight + 1;
                break;
            case 2:
                bad.domain_sets = std::span(*light_sets).first(1);
                break;
            case 3:
                invalid_sets[1] = VK_NULL_HANDLE;
                bad.domain_sets = invalid_sets;
                break;
            case 4:
                bad.domain_binding_offset = UINT32_MAX;
                break;
            case 5:
                bad.layout_id = kInvalidDescriptorLayoutId;
                break;
            case 6:
                bad.atlas_page_resolution = 0;
                break;
            case 7:
                bad.atlas_page_count = 0;
                break;
            case 8:
                bad.max_shadow_slices = 0;
                break;
            }
            const auto writes = descriptor_writes;
            auto candidate = ShadowResources::create(bad);
            assert(!candidate && isError<err::internal::InvalidArgument>(candidate.error()));
            assert(writes == descriptor_writes && buffers.size() == base_buffers && images.size() == base_images);
        }
        // The first sampler request must reject before it can enter the shared cache.
        for (const auto boundary :
             {EFailure::SAMPLER,
              EFailure::IMAGE,
              EFailure::VIEW,
              EFailure::BUFFER,
              EFailure::MAPPED,
              EFailure::FLUSH,
              EFailure::SET})
        {
            const unsigned count = boundary == EFailure::BUFFER || boundary == EFailure::MAPPED
                                       ? 8
                                       : (boundary == EFailure::FLUSH ? 6 : 1);
            for (unsigned index = 0; index < count; ++index)
            {
                failure = boundary;
                skip_rejections = index;
                set_failure_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
                const auto writes = descriptor_writes;
                const auto rejected = rejections;
                auto candidate = ShadowResources::create(info);
                failure = EFailure::NONE;
                assert(!candidate && rejections == rejected + 1);
                assert(isError<err::device::VulkanCallFailed>(candidate.error()));
                const auto expected = boundary == EFailure::MAPPED || boundary == EFailure::FLUSH
                                          ? VK_ERROR_MEMORY_MAP_FAILED
                                          : VK_ERROR_OUT_OF_DEVICE_MEMORY;
                assert(candidate.error().args[0] == encodeVkResult(expected));
                assert(descriptor_writes == writes && buffers.size() == base_buffers);
                assert(images.size() == base_images && views.size() == base_views && sets.size() == base_sets);
            }
        }
        auto owner = ShadowResources::create(info);
        assert(owner);
        auto& shadow = **owner;
        ShadowSliceGPU slice{};
        slice.light_vp.setIdentity();
        slice.light_vp(0, 0) = 17.0f;
        ShadowConfigGPU config{};
        config.total_slices = 1;
        shadow.setCachedData(7, 9, {&slice, 1}, {}, {}, config);
        const auto snapshot = shadow.findViewCache(7, 9);
        const auto image = shadow.atlasImage();
        const auto buffer = shadow.sliceBuffer();
        const auto set = shadow.descriptorSet(0);
        const auto accepted_buffers = buffers.size();
        const auto accepted_images = images.size();
        const auto accepted_sets = sets.size();
        const auto accepted_views = views.size();
        for (const auto boundary :
             {EFailure::IMAGE,
              EFailure::VIEW,
              EFailure::BUFFER,
              EFailure::MAPPED,
              EFailure::FLUSH,
              EFailure::DEVICE_IDLE})
        {
            const unsigned count = boundary == EFailure::BUFFER || boundary == EFailure::MAPPED
                                       ? 8
                                       : (boundary == EFailure::FLUSH ? 10 : 1);
            for (unsigned index = 0; index < count; ++index)
            {
                const auto writes = descriptor_writes;
                const auto idle = idle_calls;
                const auto rejected = rejections;
                failure = boundary;
                skip_rejections = index;
                const auto replaced = shadow.rebuild(32, 2, 8);
                failure = EFailure::NONE;
                assert(!replaced && rejections == rejected + 1);
                const auto expected =
                    boundary == EFailure::MAPPED || boundary == EFailure::FLUSH
                        ? VK_ERROR_MEMORY_MAP_FAILED
                        : (boundary == EFailure::DEVICE_IDLE ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_DEVICE_MEMORY);
                assert(isError<err::device::VulkanCallFailed>(replaced.error()));
                assert(replaced.error().args[0] == encodeVkResult(expected));
                assert(images.contains(image) && buffers.contains(buffer));
                assert(shadow.atlasImage() == image && shadow.sliceBuffer() == buffer);
                assert(shadow.descriptorSet(0) == set && shadow.findViewCache(7, 9) == snapshot);
                assert(shadow.atlasPageResolution() == 16 && shadow.atlasPageCount() == 1 && shadow.maxSlices() == 4);
                assert(images.size() == accepted_images && buffers.size() == accepted_buffers);
                assert(sets.size() == accepted_sets && views.size() == accepted_views && writes == descriptor_writes);
                assert(idle_calls == idle + (boundary == EFailure::DEVICE_IDLE ? 1u : 0u));
            }
        }
        assert(shadow.rebuild(32, 2, 8));
        assert(!images.contains(image) && !buffers.contains(buffer));
        assert(shadow.atlasPageResolution() == 32 && shadow.atlasPageCount() == 2 && shadow.maxSlices() == 8);
        assert(shadow.descriptorSet(0) == set && sets.size() == accepted_sets);
        assert(!shadow.findViewCache(7, 9) && snapshot->slices.size() == 1);
        assert(images.size() == accepted_images && buffers.size() == accepted_buffers);
        for (uint32_t fi = 0; fi < shadow.framesInFlight(); ++fi)
        {
            const auto read = [&](VkBuffer buffer)
            {
                VmaAllocationInfo mapping{};
                const auto& origin = buffers.at(buffer);
                vmaGetAllocationInfo(origin.first, origin.second, &mapping);
                assert(mapping.pMappedData);
                return mapping.pMappedData;
            };
            const auto* copied_slice = static_cast<const ShadowSliceGPU*>(read(shadow.sliceBuffer(fi)));
            const auto* copied_config = static_cast<const ShadowConfigGPU*>(read(shadow.configBuffer(fi)));
            assert(copied_slice->light_vp(0, 0) == 17.0f && copied_config->total_slices == 1);
            const auto* spot = static_cast<const int32_t*>(read(shadow.spotMapBuffer(fi)));
            const auto* point = static_cast<const int32_t*>(read(shadow.pointMapBuffer(fi)));
            assert(spot[0] == -1 && point[shadow.shadowMapCapacity() - 1] == -1);
        }
        assert(snapshot->slices[0].light_vp(0, 0) == 17.0f);
        const auto writes = descriptor_writes;
        const auto idle = idle_calls;
        const auto rebuilt_image = shadow.atlasImage();
        for (unsigned frame = 0; frame < 30; ++frame)
        {
            assert(shadow.rebuild(32, 2, 8));
        }
        assert(descriptor_writes == writes && idle_calls == idle && shadow.atlasImage() == rebuilt_image);
        std::puts("Shadow complete construction and replacement: exact native errors, no partial publication, accepted "
                  "backing/cache retention, retry and stable descriptors PASS");
    }
    light->reset();
    retirement.flushAll();
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using namespace lux::render;
    static_assert(!std::is_copy_constructible_v<CommandBufferOwner>);
    static_assert(!std::is_copy_assignable_v<CommandBufferOwner>);
    static_assert(std::is_nothrow_move_constructible_v<CommandBufferOwner>);
    static_assert(std::is_nothrow_move_assignable_v<CommandBufferOwner>);
    static_assert(std::is_nothrow_destructible_v<CommandBufferOwner>);
    InstanceContext instance({});
    DeviceContext device(instance);
    assert(device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
    auto resources_owner = ResourceContext::create(device);
    assert(resources_owner);
    auto& resources = **resources_owner;
    auto layout_owner = GeneralDescriptorSetLayout::create(device);
    assert(layout_owner);
    auto& layouts = **layout_owner;
    checkDescriptorRegistration(device);
    if (argc == 2 && std::string_view(argv[1]) == "--descriptor-registration")
    {
        return 0;
    }
    checkSceneDescriptorArena(device, layouts.getLayout(EDescriptorSetSlot::SCENE));
    checkHzbDescriptorFailure(device);
    if (argc == 2 && std::string_view(argv[1]) == "--scene-arena")
    {
        return 0;
    }
    DeferredDestroyQueue retirement;
    retirement.init(device.vmaAllocator(), device.logicalDevice());
    checkShadowRebuild(device, layouts, retirement);
    if (argc == 2 && std::string_view(argv[1]) == "--shadow")
    {
        return 0;
    }
    checkCanvasConstruction(device, resources, layouts, retirement);
    if (argc == 2 && std::string_view(argv[1]) == "--canvas")
    {
        return 0;
    }
    checkMdcPublication(device, layouts, retirement);
    if (argc == 2 && std::string_view(argv[1]) == "--mdc-publication")
    {
        return 0;
    }
    checkInstanceConstruction(device, layouts, retirement);
    if (argc == 2 && std::string_view(argv[1]) == "--instance-construction")
    {
        return 0;
    }
    checkInstanceGrowth(device, layouts, retirement);
    if (argc == 2 && std::string_view(argv[1]) == "--instance-growth")
    {
        return 0;
    }
    checkSparseStorage(device, retirement);
    if (argc == 2 && std::string_view(argv[1]) == "--sparse-storage")
    {
        return 0;
    }
    checkSparsePageTable(device, retirement);
    if (argc == 2 && std::string_view(argv[1]) == "--sparse-table")
    {
        return 0;
    }
    checkPagedCapacity(device, retirement);
    if (argc == 2 && std::string_view(argv[1]) == "--paged-capacity")
    {
        return 0;
    }
    checkSceneResources(device, layouts, retirement);
    if (argc == 2 && std::string_view(argv[1]) == "--scene-resources")
    {
        return 0;
    }
    static_assert(!std::is_default_constructible_v<BindlessCombinedSet>);
    static_assert(!std::is_copy_constructible_v<BindlessCombinedSet>);
    static_assert(!std::is_move_constructible_v<BindlessCombinedSet>);

    {
        DeviceContext other_device(instance);
        assert(other_device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
        auto other_resources_owner = ResourceContext::create(other_device);
        assert(other_resources_owner);
        auto& other_resources = **other_resources_owner;
        auto first = CommandBufferOwner::create(device.logicalDevice(), resources.commandPool());
        auto second = CommandBufferOwner::create(other_device.logicalDevice(), other_resources.commandPool());
        assert(first && second && commands.size() == 2);
        *first = std::move(*second);
        assert(!*second && commands.size() == 1);
        CommandBufferOwner moved(std::move(*first));
        assert(!*first && moved);
        // Destruction checks the exact originating device and pool after both moves.
    }
    assert(commands.empty());

    const auto baseline_pools = pools.size();
    const auto baseline_sets = sets.size();
    {
        static_assert(!std::is_default_constructible_v<VertexPoolRegistry>);
        static_assert(!std::is_copy_constructible_v<VertexPoolRegistry>);
        static_assert(!std::is_move_constructible_v<VertexPoolRegistry>);
        const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kVertexPoolMaxCount};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool_info.maxSets = 2;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        auto pool = DescriptorPoolOwner::create(device.logicalDevice(), pool_info);
        assert(pool);
        const std::array set_layouts{
            layouts.getLayout(EDescriptorSetSlot::VERTEX_POOL),
            layouts.getLayout(EDescriptorSetSlot::VERTEX_POOL)
        };
        std::array<VkDescriptorSet, 2> targets{};
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = pool->get();
        allocation.descriptorSetCount = 2;
        allocation.pSetLayouts = set_layouts.data();
        assert(allocateSets(device.logicalDevice(), &allocation, targets.data()) == VK_SUCCESS);
        const auto original_targets = targets;
        const auto original_writes = descriptor_writes;
        for (unsigned index = 0; index < 3; ++index)
        {
            auto invalid_targets = targets;
            if (index == 1)
            {
                invalid_targets.fill(VK_NULL_HANDLE);
            }
            if (index == 2)
            {
                invalid_targets[1] = VK_NULL_HANDLE;
            }
            const auto input =
                index == 0 ? std::span<const VkDescriptorSet>{} : std::span<const VkDescriptorSet>(invalid_targets);
            ResourceRegistry registry;
            auto rejected = VertexPoolRegistry::create(device, input, 0);
            assert(!rejected && isError<err::descriptor::InvalidVertexPoolTarget>(rejected.error()));
            assert(!registry.find<VertexPoolRegistry>() && descriptor_writes == original_writes);
        }
        {
            DeviceContext inactive(instance);
            auto rejected = VertexPoolRegistry::create(inactive, targets, 0);
            assert(!rejected && isError<err::descriptor::InvalidVertexPoolTarget>(rejected.error()));
        }
        {
            ResourceRegistry registry;
            auto candidate = VertexPoolRegistry::create(device, targets, 0);
            assert(candidate && !registry.find<VertexPoolRegistry>());
            auto* original = candidate->get();
            auto published = registry.insert(std::move(*candidate));
            assert(published.get() == original && registry.find<VertexPoolRegistry>() == original);
            assert(descriptor_writes == original_writes);
            targets.fill(VK_NULL_HANDLE); // The accepted write target owns its small handle array.
            std::array<std::unique_ptr<TransientVertexSource>, kVertexPoolMaxCount + 1> sources;
            for (auto& source : sources)
            {
                auto candidate_source = TransientVertexSource::create({&device, 4096, 0, 16});
                assert(candidate_source);
                source = std::move(*candidate_source);
            }
            buffer_writes.clear();
            trace_buffer_writes = true;
            for (unsigned id = 0; id < kVertexPoolMaxCount; ++id)
            {
                assert(original->registerSource(*sources[id]) == id);
                assert(original->isRegistered(id) && sources[id]->bindlessPoolId() == id);
                for (unsigned fi = 0; fi < 2; ++fi)
                {
                    const auto& write = buffer_writes[id * 2 + fi];
                    assert(write.device == device.logicalDevice() && write.set == original_targets[fi]);
                    assert(write.binding == 0 && write.index == id && write.buffer == sources[id]->buffer());
                }
            }
            const auto writes_at_capacity = buffer_writes.size();
            assert(original->registerSource(*sources.back()) == ~0u);
            assert(sources.back()->bindlessPoolId() == ~0u && buffer_writes.size() == writes_at_capacity);
            original->unregisterSource(3);
            assert(!original->isRegistered(3) && sources[3]->bindlessPoolId() == ~0u);
            assert(original->registerSource(*sources.back()) == 3);
            original->refreshSource(3);
            assert(buffer_writes.size() == writes_at_capacity + 4);
            assert(buffer_writes.back().buffer == sources.back()->buffer());
            original->refreshSource(~0u);
            original->unregisterSource(~0u);
            assert(!original->isRegistered(~0u));
            for (unsigned id = 0; id < kVertexPoolMaxCount; ++id)
            {
                original->unregisterSource(id);
            }
            for (const auto& source : sources)
            {
                assert(source->bindlessPoolId() == ~0u);
            }
            trace_buffer_writes = false;
            buffer_writes.clear();
        }
        assert(buffers.empty() && sets.contains(original_targets[0]) && sets.contains(original_targets[1]));
        std::puts("Vertex pool complete target: rejection before publication, per-frame writes, capacity/reuse, borrow "
                  "lifetime PASS");
        static_assert(!std::is_default_constructible_v<TransientVertexSource>);
        static_assert(!std::is_copy_constructible_v<TransientVertexSource>);
        static_assert(!std::is_move_constructible_v<TransientVertexSource>);
        static_assert(!std::is_default_constructible_v<SkinningResources>);
        static_assert(!std::is_copy_constructible_v<SkinningResources>);
        static_assert(!std::is_move_constructible_v<SkinningResources>);
        auto vertices = VertexPoolRegistry::create(device, original_targets, 0);
        assert(vertices);
        DeviceContext inactive_device(instance);
        for (unsigned index = 0; index < 7; ++index)
        {
            TransientVertexSource::CreateInfo config{&device, 4096, 0, 16};
            switch (index)
            {
            case 0:
                config.device_context = nullptr;
                break;
            case 1:
                config.capacity_bytes = 0;
                break;
            case 2:
                config.capacity_bytes = 8;
                break;
            case 3:
                config.vertex_stride = 0;
                break;
            case 4:
                config.layout_id = kInvalidVertexLayoutId;
                break;
            case 5:
                config.capacity_bytes = (VkDeviceSize(UINT32_MAX) + 1) * 16;
                break;
            case 6:
                config.device_context = &inactive_device;
                break;
            }
            auto candidate = TransientVertexSource::create(config);
            assert(!candidate && isError<err::memory::InvalidTransientVertexConfiguration>(candidate.error()));
            assert(buffers.empty());
        }
        failure = EFailure::BUFFER;
        auto failed_source = TransientVertexSource::create({&device, 4096, 0, 16});
        assert(!failed_source && isError<err::device::VulkanCallFailed>(failed_source.error()));
        assert(failed_source.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        assert(buffers.empty());
        failure = EFailure::NONE;
        {
            auto source = TransientVertexSource::create({&device, 4096, 0, 16});
            assert(source && (*source)->buffer() && (*source)->layout() == 0);
            assert((*vertices)->registerSource(**source) == 0);
            auto first = (*source)->allocate(10);
            auto second = (*source)->allocate(246);
            assert(first.valid() && first.pool_id == 0 && first.vertex_base == 0 && first.vertex_count == 10);
            assert(second.valid() && second.vertex_base == 10 && second.vertex_count == 246);
            assert(!(*source)->allocate(1).valid() && !(*source)->allocate(UINT32_MAX).valid());
            (*source)->beginFrame();
            assert((*source)->allocate(256).vertex_base == 0);
            (*vertices)->unregisterSource(0);
        }
        assert(buffers.empty());
        SkinningResources::CreateInfo config{};
        config.device_context = &device;
        config.vertex_pool_registry = vertices->get();
        config.layout_id = 0;
        config.vertex_stride = 16;
        config.max_bones = 16;
        config.max_dispatches = 2;
        config.output_pool_bytes = 4096;
        for (auto boundary : {EFailure::BUFFER, EFailure::MAPPED})
        {
            const unsigned count = 2 * kMaxFramesInFlight + unsigned(boundary == EFailure::BUFFER);
            for (unsigned index = 0; index < count; ++index)
            {
                const auto old_writes = descriptor_writes;
                const auto old_rejections = rejections;
                failure = boundary;
                skip_rejections = index;
                ResourceRegistry registry;
                auto candidate = SkinningResources::create(config);
                assert(!candidate && !registry.find<SkinningResources>());
                const auto expected =
                    boundary == EFailure::MAPPED ? VK_ERROR_MEMORY_MAP_FAILED : VK_ERROR_OUT_OF_DEVICE_MEMORY;
                assert(isError<err::device::VulkanCallFailed>(candidate.error()));
                assert(candidate.error().args[0] == encodeVkResult(expected));
                assert(buffers.empty() && rejections == old_rejections + 1);
                assert(descriptor_writes == old_writes && !(*vertices)->isRegistered(0));
            }
        }
        failure = EFailure::NONE;
        for (unsigned index = 0; index < 4; ++index)
        {
            auto bad = config;
            switch (index)
            {
            case 0:
                bad.device_context = nullptr;
                break;
            case 1:
                bad.vertex_pool_registry = nullptr;
                break;
            case 2:
                bad.max_bones = 0;
                break;
            case 3:
                bad.max_dispatches = 0;
                break;
            }
            auto candidate = SkinningResources::create(bad);
            assert(!candidate && isError<err::memory::InvalidSkinningConfiguration>(candidate.error()));
            assert(buffers.empty() && !(*vertices)->isRegistered(0));
        }
        for (unsigned index = 0; index < 3; ++index)
        {
            auto bad = config;
            if (index == 0)
            {
                bad.vertex_stride = 0;
            }
            if (index == 1)
            {
                bad.output_pool_bytes = 8;
            }
            if (index == 2)
            {
                bad.layout_id = kInvalidVertexLayoutId;
            }
            auto candidate = SkinningResources::create(bad);
            assert(!candidate && isError<err::memory::InvalidTransientVertexConfiguration>(candidate.error()));
            assert(buffers.empty() && !(*vertices)->isRegistered(0));
        }
        {
            std::array<std::unique_ptr<TransientVertexSource>, kVertexPoolMaxCount> occupied;
            for (unsigned id = 0; id < occupied.size(); ++id)
            {
                auto source = TransientVertexSource::create({&device, 4096, 0, 16});
                assert(source);
                occupied[id] = std::move(*source);
                assert((*vertices)->registerSource(*occupied[id]) == id);
            }
            const auto old_writes = descriptor_writes;
            auto failed = SkinningResources::create(config);
            assert(!failed && isError<err::frame::VertexPoolRegistryFull>(failed.error()));
            assert(buffers.size() == occupied.size() && descriptor_writes == old_writes);
            (*vertices)->unregisterSource(7);
            {
                auto retry = SkinningResources::create(config);
                assert(retry && (*retry)->outputPoolId() == 7);
                assert(buffers.size() == occupied.size() + 2 * kMaxFramesInFlight + 1);
            }
            assert(!(*vertices)->isRegistered(7) && buffers.size() == occupied.size());
            for (unsigned id = 0; id < occupied.size(); ++id)
            {
                (*vertices)->unregisterSource(id);
            }
        }
        assert(buffers.empty());
        {
            ResourceRegistry registry;
            auto candidate = SkinningResources::create(config);
            assert(candidate && !registry.find<SkinningResources>());
            auto* original = candidate->get();
            auto published = registry.insert(std::move(*candidate));
            assert(published.get() == original && registry.find<SkinningResources>() == original);
            assert(original->outputPoolId() == 0 && original->maxDispatches() == 2);
            assert(buffers.size() == 2 * kMaxFramesInFlight + 1);
            std::array<BoneMatrixGpu, 16> bones{};
            bones[0].m[0] = 3.f;
            for (unsigned frame = 0; frame < kMaxFramesInFlight; ++frame)
            {
                original->beginFrameIfNew(frame);
                assert(original->currentFrameIndex() == frame && original->dispatches().empty());
                assert(original->uploadBonePalette(bones.data(), 16) == 0);
                assert(original->uploadBonePalette(bones.data(), 1) == ~0u);
                assert(original->uploadBonePalette(nullptr, 1) == ~0u);
                assert(original->queueDispatch(0, 10, 65, 0, 2).vertex_base == 0);
                assert(original->queueDispatch(0, 80, 64, 2, 2).vertex_base == 65);
                original->beginFrameIfNew(frame);
                assert(original->dispatches().size() == 2 && original->uploadDispatches() == 3);
                const auto& palette_origin = buffers.at(original->bonePaletteBuffer(frame));
                VmaAllocationInfo palette_info{};
                vmaGetAllocationInfo(palette_origin.first, palette_origin.second, &palette_info);
                assert(std::memcmp(palette_info.pMappedData, bones.data(), sizeof(bones)) == 0);
                const auto& params_origin = buffers.at(original->dispatchParamsBuffer(frame));
                VmaAllocationInfo params_info{};
                vmaGetAllocationInfo(params_origin.first, params_origin.second, &params_info);
                const auto* parameters = static_cast<const SkinDispatchParams*>(params_info.pMappedData);
                assert(parameters[0].workgroup_start == 0 && parameters[1].workgroup_start == 2);
                assert(parameters[1].in_base == 80 && parameters[1].out_base == 65 && parameters[1].palette_base == 2);
                assert(!original->queueDispatch(0, 0, 200, 0, 2).valid());
                assert(original->dispatches().size() == 2);
                assert(original->queueDispatch(0, 0, 1, 0, 2).valid());
                assert(original->uploadDispatches() == 0); // Preserve the original whole-batch capacity rejection.
            }
            original->beginFrameIfNew(kMaxFramesInFlight);
            assert(original->dispatches().empty() && original->uploadBonePalette(bones.data(), 1) == 0);
        }
        assert(buffers.empty() && !(*vertices)->isRegistered(0));
        std::puts("Skinning complete backing: native allocation/mapping rollback, no early registration, capacity "
                  "retry, rings and dispatch identity PASS");
    }
    assert(pools.size() == baseline_pools && sets.size() == baseline_sets);
    const bool vertex_only =
        argc > 1 && (std::string_view(argv[1]) == "--vertex-pool" || std::string_view(argv[1]) == "--skinning");
    if (vertex_only)
    {
        return 0;
    }
    for (auto boundary :
         {EFailure::NONE,
          EFailure::POOL,
          EFailure::SET,
          EFailure::IMAGE,
          EFailure::VIEW,
          EFailure::SAMPLER,
          EFailure::ALLOCATE,
          EFailure::BEGIN,
          EFailure::END,
          EFailure::FENCE,
          EFailure::SUBMIT,
          EFailure::WAIT})
    {
        failure = boundary;
        rejections = 0;
        const auto old_idle_calls = idle_calls;
        {
            BindlessSetCreateInfo info{};
            info.resource_context = &resources;
            info.deferred_queue = &retirement;
            info.descriptor_set_layout = layouts.getLayout(TGetBindingSet<ETextureSetBindings>::value);
            info.layout_max_capacity = layouts.bindless2DCount();
            info.initial_capacity = 8;
            auto candidate = BindlessCombinedSet::create(info);
            std::printf(
                "bindless construction: boundary=%u accepted=%d expected=%d\n",
                static_cast<unsigned>(boundary),
                static_cast<bool>(candidate),
                boundary == EFailure::NONE
            );
            assert(static_cast<bool>(candidate) == (boundary == EFailure::NONE));
            if (!candidate)
            {
                assert(isError<err::device::VulkanCallFailed>(candidate.error()));
                const auto error =
                    boundary == EFailure::WAIT
                        ? VK_ERROR_OUT_OF_HOST_MEMORY
                        : (boundary == EFailure::SET ? VK_ERROR_OUT_OF_POOL_MEMORY : VK_ERROR_OUT_OF_DEVICE_MEMORY);
                assert(candidate.error().args[0] == encodeVkResult(error));
            }
            else
            {
                assert((*candidate)->descriptorSet() && (*candidate)->capacity() == 8);
            }
        }
        assert(pools.size() == baseline_pools && sets.size() == baseline_sets);
        assert(images.empty() && views.empty() && samplers.empty());
        assert(rejections == (boundary == EFailure::NONE ? 0u : 1u));
        if (!commands.empty() || !fences.empty())
        {
            std::fprintf(
                stderr,
                "boundary=%u leaked commands=%zu fences=%zu\n",
                static_cast<unsigned>(boundary),
                commands.size(),
                fences.size()
            );
            // Diagnostic cleanup only; the failing result remains a real failure.
            for (const auto& [cmd, origin] : commands)
            {
                vkFreeCommandBuffers(origin.device, origin.pool, 1, &cmd);
            }
            for (const auto& [fence, dev] : fences)
            {
                vkDestroyFence(dev, fence, nullptr);
            }
            return 1;
        }
        if (boundary == EFailure::WAIT)
        {
            assert(idle_calls == old_idle_calls + 1);
        }
    }

    {
        static_assert(!std::is_default_constructible_v<LightResources>);
        static_assert(!std::is_copy_constructible_v<LightResources>);
        static_assert(!std::is_move_constructible_v<LightResources>);
        const std::array pool_sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}
        };
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool_info.maxSets = 2;
        pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        failure = EFailure::NONE;
        auto pool = DescriptorPoolOwner::create(device.logicalDevice(), pool_info);
        assert(pool);
        const std::array set_layouts{
            layouts.getLayout(TGetBindingSet<ELightSetBindings>::value),
            layouts.getLayout(TGetBindingSet<ELightSetBindings>::value)
        };
        std::array<VkDescriptorSet, 2> light_sets{};
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = pool->get();
        allocation.descriptorSetCount = 2;
        allocation.pSetLayouts = set_layouts.data();
        assert(allocateSets(device.logicalDevice(), &allocation, light_sets.data()) == VK_SUCCESS);
        DescriptorService descriptors(device.logicalDevice());
        LightResources::CreateInfo config{};
        config.ssbo_config = SSBOInitConfig{
            .device_context = &device,
            .deferred_queue = &retirement,
            .initial_dense_capacity = 16,
            .slices = 2
        };
        config.descriptor_svc = &descriptors;
        config.domain_sets = light_sets;
        for (auto boundary : {EFailure::BUFFER, EFailure::MAPPED, EFailure::IMAGE, EFailure::VIEW, EFailure::SAMPLER})
        {
            const unsigned attempts = boundary == EFailure::BUFFER || boundary == EFailure::MAPPED ? 4 : 1;
            for (unsigned index = 0; index < attempts; ++index)
            {
                failure = boundary;
                skip_rejections = index;
                const auto original_writes = descriptor_writes;
                const auto original_rejections = rejections;
                ResourceRegistry registry;
                auto candidate = LightResources::create(config);
                assert(!candidate && registry.find<LightResources>() == nullptr);
                assert(rejections == original_rejections + 1);
                const auto expected =
                    boundary == EFailure::MAPPED ? VK_ERROR_MEMORY_MAP_FAILED : VK_ERROR_OUT_OF_DEVICE_MEMORY;
                assert(isError<err::device::VulkanCallFailed>(candidate.error()));
                assert(candidate.error().args[0] == encodeVkResult(expected));
                assert(descriptor_writes == original_writes);
                assert(images.empty() && views.empty() && samplers.empty());
                failure = EFailure::NONE;
                retirement.flushAll();
                assert(buffers.empty());
            }
        }
        for (unsigned index = 0; index < 7; ++index)
        {
            auto bad = config;
            auto invalid_sets = light_sets;
            switch (index)
            {
            case 0:
                bad.ssbo_config.device_context = nullptr;
                break;
            case 1:
                bad.descriptor_svc = nullptr;
                break;
            case 2:
                bad.ssbo_config.slices = 0;
                break;
            case 3:
                bad.ssbo_config.slices = kMaxFramesInFlight + 1;
                break;
            case 4:
                bad.domain_sets = std::span(light_sets).first(1);
                break;
            case 5:
                invalid_sets[1] = VK_NULL_HANDLE;
                bad.domain_sets = invalid_sets;
                break;
            case 6:
                bad.domain_binding_offset = UINT32_MAX;
                break;
            }
            const auto original_writes = descriptor_writes;
            auto candidate = LightResources::create(bad);
            assert(!candidate && isError<err::memory::InvalidLightConfiguration>(candidate.error()));
            assert(descriptor_writes == original_writes);
            assert(images.empty() && views.empty() && samplers.empty() && buffers.empty());
        }
        {
            ResourceRegistry registry;
            auto candidate = LightResources::create(config);
            assert(candidate);
            auto* original = candidate->get();
            assert(registry.insert(std::move(*candidate)));
            assert(registry.find<LightResources>() == original && original->framesInFlight() == 2);
            assert(images.size() == 1 && views.size() == 1 && samplers.size() == 1 && buffers.size() == 4);
            original->provideShadingInput(EShadingInputSlot{}, views.begin()->first);
            original->provideShadingInput(EShadingInputSlot{}, VK_NULL_HANDLE);
            const VLightDescriptor point = PointLightDesc{};
            auto light = original->submit(point);
            assert(light && original->lightCount(ELightSetBindings::LIGHT_POINT) == 1);
            assert(original->beginFadeIn(*light, 1.0f, 1.0f));
            original->advanceIntensityTransitions(1.5f);
            float intensity{};
            original->forEachLight<PointLightGPU>([&](unsigned, const PointLightGPU& value)
                                                  { intensity = value.intensity; });
            assert(intensity > 0.0f);
            original->remove(*light);
            auto reused = original->submit(point);
            assert(reused && reused->index == light->index && reused->gen != light->gen);
            assert(!original->modify(*light, point).ok());
            original->remove(*reused);
            for (const VLightDescriptor data :
                 {VLightDescriptor{DirectionalLightDesc{}},
                  point,
                  VLightDescriptor{SpotLightDesc{}},
                  VLightDescriptor{AreaLightDesc{}}})
            {
                std::vector<LightHandle> live;
                // TGpuBuffer's minimum capacity is 64; reject the first growth of each family.
                for (unsigned i = 0; i < 64; ++i)
                {
                    auto next = original->submit(data);
                    assert(next);
                    live.push_back(*next);
                }
                failure = EFailure::BUFFER;
                skip_rejections = 0;
                auto rejected = original->submit(data);
                assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
                failure = EFailure::NONE;
                for (const auto handle : live)
                {
                    assert(original->modify(handle, data).ok());
                }
                auto next = original->submit(data);
                assert(next);
                original->remove(*next);
                for (const auto handle : live)
                {
                    original->remove(handle);
                }
                retirement.flushAll();
                assert(buffers.size() == 4);
            }
            // Record the original first-frame GPU initialization twice; only its original one-time state applies.
            auto command = CommandBufferOwner::create(device.logicalDevice(), resources.commandPool());
            assert(command);
            VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            assert(begin(command->get(), &begin_info) == VK_SUCCESS);
            original->postTransfer(command->get());
            original->postTransfer(command->get());
            assert(end(command->get()) == VK_SUCCESS);
            VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            auto fence = FenceOwner::create(device.logicalDevice(), fence_info);
            assert(fence);
            const auto cmd = command->get();
            VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submission.commandBufferCount = 1;
            submission.pCommandBuffers = &cmd;
            assert(submit(device.graphicsQueue(), 1, &submission, fence->get()) == VK_SUCCESS);
            const auto fence_handle = fence->get();
            assert(wait(device.logicalDevice(), 1, &fence_handle, VK_TRUE, UINT64_MAX) == VK_SUCCESS);
        }
        assert(images.empty() && views.empty() && samplers.size() == 1);
        assert(retirement.pendingCount() == 4);
        retirement.flushAll();
        assert(buffers.empty());
        std::puts("Light complete construction: native rejection/cleanup, exact errors, no premature descriptors, "
                  "registry, handle generations, four-family growth and actual GPU default clear PASS");
    }
    assert(images.empty() && views.empty() && samplers.empty());
    assert(pools.size() == baseline_pools && sets.size() == baseline_sets);
    if (argc > 1 && std::string_view(argv[1]) == "--light")
    {
        return 0;
    }

    failure = EFailure::NONE;
    BindlessSetCreateInfo info{};
    info.resource_context = &resources;
    info.deferred_queue = &retirement;
    info.descriptor_set_layout = layouts.getLayout(TGetBindingSet<ETextureSetBindings>::value);
    info.layout_max_capacity = layouts.bindless2DCount();
    info.initial_capacity = 8;
    {
        static_assert(!std::is_default_constructible_v<MaterialResources>);
        static_assert(!std::is_copy_constructible_v<MaterialResources>);
        static_assert(!std::is_move_constructible_v<MaterialResources>);
        TextureResources::CreateInfo texture_config{};
        texture_config.combined_ci = info;
        texture_config.cube_max_capacity = layouts.bindlessCubeCount();
        auto textures = TextureResources::create(texture_config);
        assert(textures);
        const auto texture_buffer_count = buffers.size();
        const auto texture_set_count = sets.size();
        auto catalog = builtinTextureSamplingRepresentationCatalog();
        MaterialResources::CreateInfo config{};
        config.ssbo_config = SSBOInitConfig{
            .device_context = &device,
            .deferred_queue = &retirement,
            .initial_dense_capacity = 16,
            .slices = 2
        };
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 100};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool_info.maxSets = 2;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &size;
        auto pool = DescriptorPoolOwner::create(device.logicalDevice(), pool_info);
        assert(pool);
        config.descriptor_pool = pool->get();
        config.set_layout = layouts.getLayout(EDescriptorSetSlot::MATERIAL);
        config.texture_sampling_catalog = &catalog;
        config.textures = textures->get();
        for (auto boundary : {EFailure::BUFFER, EFailure::MAPPED, EFailure::SET})
        {
            const unsigned attempts = boundary == EFailure::SET ? 1 : 5;
            for (unsigned index = 0; index < attempts; ++index)
            {
                failure = boundary;
                skip_rejections = index;
                const auto old_rejections = rejections;
                ResourceRegistry registry;
                auto candidate = MaterialResources::create(config);
                assert(!candidate && registry.find<MaterialResources>() == nullptr);
                assert(rejections == old_rejections + 1);
                const auto expected =
                    boundary == EFailure::MAPPED
                        ? VK_ERROR_MEMORY_MAP_FAILED
                        : (boundary == EFailure::SET ? VK_ERROR_OUT_OF_POOL_MEMORY : VK_ERROR_OUT_OF_DEVICE_MEMORY);
                assert(isError<err::device::VulkanCallFailed>(candidate.error()));
                assert(candidate.error().args[0] == encodeVkResult(expected));
                const auto retired = boundary == EFailure::SET ? 5 : index + unsigned(boundary == EFailure::MAPPED);
                assert(retirement.pendingCount() == retired);
                failure = EFailure::NONE;
                retirement.flushAll();
                assert(buffers.size() == texture_buffer_count && sets.size() == texture_set_count);
            }
        }
        for (unsigned index = 0; index < 7; ++index)
        {
            auto bad = config;
            TextureSamplingRepresentationCatalog empty_catalog;
            switch (index)
            {
            case 0:
                bad.texture_sampling_catalog = nullptr;
                break;
            case 1:
                bad.textures = nullptr;
                break;
            case 2:
                bad.descriptor_pool = VK_NULL_HANDLE;
                break;
            case 3:
                bad.set_layout = VK_NULL_HANDLE;
                break;
            case 4:
                bad.ssbo_config.slices = 0;
                break;
            case 5:
                bad.ssbo_config.slices = kMaxFramesInFlight + 1;
                break;
            case 6:
                bad.texture_sampling_catalog = &empty_catalog;
                break;
            }
            auto rejected = MaterialResources::create(bad);
            assert(!rejected && isError<err::memory::InvalidMaterialConfiguration>(rejected.error()));
            assert(buffers.size() == texture_buffer_count && retirement.pendingCount() == 0);
        }
        for (unsigned index = 0; index < 3; ++index)
        {
            auto bad = config;
            switch (index)
            {
            case 0:
                bad.ssbo_config.device_context = nullptr;
                break;
            case 1:
                bad.ssbo_config.deferred_queue = nullptr;
                break;
            case 2:
                bad.ssbo_config.initial_dense_capacity = 0;
                break;
            }
            auto rejected = MaterialResources::create(bad);
            assert(!rejected && isError<err::memory::InvalidBufferConfiguration>(rejected.error()));
            assert(buffers.size() == texture_buffer_count && retirement.pendingCount() == 0);
        }
        {
            ResourceRegistry registry;
            auto candidate = MaterialResources::create(config);
            assert(candidate && registry.find<MaterialResources>() == nullptr);
            auto* original = candidate->get();
            auto published = registry.insert(std::move(*candidate));
            assert(published.get() == original && registry.find<MaterialResources>() == original);
            assert(original->descriptorSet(0) && original->descriptorSet(1));
            assert(original->descriptorSet(0) != original->descriptorSet(1));
            assert(original->graphMaterialAddress(0) && original->graphMaterialCapacity() >= 16);
            GraphMaterialData data{};
            data.param_count = 1;
            data.params[0][0] = 17;
            auto handle = original->submitGraph(data);
            assert(handle && original->slotRecord(*handle));
            auto bad = data;
            bad.tex_mask = 1;
            assert(!original->submitGraph(bad));
            assert(original->retainForInstance(*handle));
            original->remove(*handle);
            assert(original->slotRecord(*handle) && !original->retainForInstance(*handle));
            original->releaseFromInstance(*handle);
            assert(!original->slotRecord(*handle));
            auto reused = original->submitGraph(data);
            assert(reused && reused->index == handle->index && reused->gen != handle->gen);
            data.params[0][0] = 29;
            assert(original->modifyGraph(*reused, data).ok());
            original->remove(*reused);
            assert(!original->slotRecord(*reused));
            std::vector<MaterialHandle> live;
            const auto old_capacity = original->graphMaterialCapacity();
            for (unsigned index = 0; index < old_capacity; ++index)
            {
                auto next = original->submitGraph(data);
                assert(next);
                live.push_back(*next);
            }
            const auto old_address = original->graphMaterialAddress(0);
            const auto old_buckets = original->variantBucketCount();
            for (auto boundary : {EFailure::BUFFER, EFailure::MAPPED, EFailure::FLUSH})
            {
                failure = boundary;
                skip_rejections = 0;
                const auto rejected = original->submitGraph(data, {}, {}, 77);
                assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
                const auto expected =
                    boundary == EFailure::BUFFER ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_MEMORY_MAP_FAILED;
                assert(rejected.error().args[0] == encodeVkResult(expected));
                assert(original->graphMaterialCapacity() == old_capacity);
                assert(original->graphMaterialAddress(0) == old_address);
                assert(original->variantBucketCount() == old_buckets);
                for (const auto h : live)
                {
                    assert(original->slotRecord(h) && original->slotRecord(h)->local_slot.isValid());
                    assert(original->modifyGraph(h, data).ok());
                }
                failure = EFailure::NONE;
                retirement.flushAll();
                assert(buffers.size() == texture_buffer_count + 5);
            }
            const auto admitted = original->submitGraph(data, {}, {}, 77);
            assert(admitted && admitted->index == live.back().index + 1);
            assert(original->graphMaterialCapacity() > old_capacity);
            assert(original->variantBucketCount() == old_buckets + 1);
            for (const auto h : live)
            {
                assert(original->modifyGraph(h, data).ok());
                original->remove(h);
            }
            original->remove(*admitted);
            assert(retirement.pendingCount() == 1);
            retirement.flushAll();
            std::puts("Material growth: allocation/map/flush failures preserve handles, capacity and buckets; retry "
                      "publishes once PASS");
        }
        assert(retirement.pendingCount() == 5 && buffers.size() == texture_buffer_count + 5);
        retirement.flushAll();
        pool->reset();
        assert(buffers.size() == texture_buffer_count && sets.size() == texture_set_count);
        textures->reset();
        retirement.flushAll();
        assert(buffers.empty() && images.empty() && views.empty() && samplers.empty());
        assert(pools.size() == baseline_pools && sets.size() == baseline_sets);
        std::puts("Material complete construction: five allocation/mapping failures, descriptor failure, exact errors, "
                  "retry, registry publication and retained material generations PASS");
        if (argc > 1 && std::string_view(argv[1]) == "--material")
        {
            return 0;
        }
    }
    {
        static_assert(!std::is_default_constructible_v<TextureResources>);
        static_assert(!std::is_copy_constructible_v<TextureResources>);
        static_assert(!std::is_move_constructible_v<TextureResources>);
        TextureResources::CreateInfo config{};
        config.combined_ci = info;
        config.combined_ci.frames_in_flight = 2;
        config.cube_max_capacity = layouts.bindlessCubeCount();
        acquisition = fail_acquisition = 0;
        measure_acquisitions = true;
        {
            auto candidate = TextureResources::create(config);
            assert(candidate);
            assert((*candidate)->descriptorSet());
            assert((*candidate)->bindlessSet2D().count() == 1);
            assert((*candidate)->mipFeedbackAddress(0) && (*candidate)->mipFeedbackAddress(1));
            assert((*candidate)->mipFeedbackAddress(0) != (*candidate)->mipFeedbackAddress(1));
        }
        const auto boundary_count = acquisition;
        measure_acquisitions = false;
        assert(boundary_count > 25 && retirement.pendingCount() == 2 && buffers.size() == 2);
        retirement.flushAll();
        for (unsigned point = 1; point <= boundary_count; ++point)
        {
            ResourceRegistry registry;
            acquisition = 0;
            fail_acquisition = point;
            rejected_boundary = EFailure::NONE;
            const auto old_rejections = rejections;
            measure_acquisitions = true;
            auto candidate = TextureResources::create(config);
            measure_acquisitions = false;
            assert(!candidate && registry.find<TextureResources>() == nullptr);
            assert(rejections == old_rejections + 1);
            const auto expected =
                rejected_boundary == EFailure::SET
                    ? VK_ERROR_OUT_OF_POOL_MEMORY
                    : (rejected_boundary == EFailure::WAIT
                           ? VK_ERROR_OUT_OF_HOST_MEMORY
                           : (rejected_boundary == EFailure::MAPPED || rejected_boundary == EFailure::FLUSH
                                  ? VK_ERROR_MEMORY_MAP_FAILED
                                  : VK_ERROR_OUT_OF_DEVICE_MEMORY));
            assert(isError<err::device::VulkanCallFailed>(candidate.error()));
            assert(candidate.error().args[0] == encodeVkResult(expected));
            assert(pools.size() == baseline_pools && sets.size() == baseline_sets);
            assert(images.empty() && views.empty() && samplers.empty() && buffers.empty());
            assert(commands.empty() && fences.empty() && retirement.pendingCount() == 0);
        }
        for (unsigned invalid = 0; invalid < 6; ++invalid)
        {
            auto bad = config;
            switch (invalid)
            {
            case 0:
                bad.combined_ci.resource_context = nullptr;
                break;
            case 1:
                bad.combined_ci.deferred_queue = nullptr;
                break;
            case 2:
                bad.combined_ci.descriptor_set_layout = VK_NULL_HANDLE;
                break;
            case 3:
                bad.combined_ci.frames_in_flight = 0;
                break;
            case 4:
                bad.cube_max_capacity = 0;
                break;
            case 5:
                bad.combined_ci.layout_max_capacity = UINT32_MAX;
                break;
            }
            auto rejected = TextureResources::create(bad);
            assert(!rejected && isError<err::memory::InvalidTextureConfiguration>(rejected.error()));
        }
        {
            auto bad = config;
            bad.fallback_pixel = lux::rdesc::Texture{};
            auto rejected = TextureResources::create(bad);
            assert(!rejected && isError<err::asset::Invalid>(rejected.error()));
            assert(buffers.empty() && images.empty() && retirement.pendingCount() == 0);
        }
        {
            ResourceRegistry registry;
            auto candidate = TextureResources::create(config);
            assert(candidate && registry.find<TextureResources>() == nullptr);
            const auto original = candidate->get();
            auto published = registry.insert(std::move(*candidate));
            assert(published.get() == original && registry.find<TextureResources>() == original);
            assert(original->mipFeedbackCapacity() == config.combined_ci.layout_max_capacity);
            const auto fallback = original->fallbackBindlessIndex();
            const TextureHandle handle{fallback, original->bindlessSet2D().genAt(fallback)};
            const auto remote = original->publishTexture(handle);
            assert(remote.isValid() && original->resolveTexture(remote) == handle);
            assert(original->imageView(handle) && original->sampler(handle));
            original->unpublish(remote);
            assert(!original->resolve(remote));
        }
        assert(retirement.pendingCount() == 2 && buffers.size() == 2);
        retirement.flushAll();
        assert(buffers.empty() && images.empty() && views.empty() && samplers.empty());
        assert(pools.size() == baseline_pools && sets.size() == baseline_sets);
        std::printf(
            "TextureResources complete factory: %u native boundaries, exact errors, unpublished rollback and registry "
            "adoption PASS\n",
            boundary_count
        );
        if (argc > 1 && std::string_view(argv[1]) == "--texture-resource")
        {
            return 0;
        }
    }
    {
        const std::array<std::byte, 4> pixels{};
        lux::rdesc::TextureInfo texture_info{};
        texture_info.width = texture_info.height = 1;
        texture_info.channel = 4;
        auto texture = lux::rdesc::Texture::copyOf(texture_info, pixels);
        assert(texture);
        const std::array faces{*texture, *texture, *texture, *texture, *texture, *texture};
        for (unsigned kind = 0; kind < 3; ++kind)
        {
            auto texture_config = info;
            if (kind == 2)
            {
                texture_config.binding = static_cast<unsigned>(ETextureSetBindings::CUBE_TEXTURES);
                texture_config.view_type = VK_IMAGE_VIEW_TYPE_CUBE;
                texture_config.layout_max_capacity = layouts.bindlessCubeCount();
            }
            for (const auto boundary :
                 {EFailure::IMAGE,
                  EFailure::VIEW,
                  EFailure::SAMPLER,
                  EFailure::BUFFER,
                  EFailure::MAPPED,
                  EFailure::FLUSH,
                  EFailure::NONE})
            {
                failure = EFailure::NONE;
                auto candidate = BindlessCombinedSet::create(texture_config);
                assert(candidate);
                auto& set = **candidate;
                const auto baseline_images = images.size();
                const auto baseline_views = views.size();
                const auto baseline_samplers = samplers.size();
                const auto old_rejections = rejections;
                failure = boundary;
                auto result = kind == 0 ? set.addTexture(*texture)
                                        : (kind == 1 ? set.addPersistentTexture(1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM)
                                                     : set.addCubeTexture(faces.data()));
                assert(static_cast<bool>(result) == (boundary == EFailure::NONE));
                if (!result)
                {
                    const auto expected = boundary == EFailure::MAPPED || boundary == EFailure::FLUSH
                                              ? VK_ERROR_MEMORY_MAP_FAILED
                                              : VK_ERROR_OUT_OF_DEVICE_MEMORY;
                    assert(isError<err::device::VulkanCallFailed>(result.error()));
                    assert(result.error().args[0] == encodeVkResult(expected));
                    assert(rejections == old_rejections + 1);
                    assert(set.count() == 0 && images.size() == baseline_images);
                    assert(views.size() == baseline_views && samplers.size() == baseline_samplers && buffers.empty());
                    failure = EFailure::NONE;
                    result = kind == 0 ? set.addTexture(*texture)
                                       : (kind == 1 ? set.addPersistentTexture(1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM)
                                                    : set.addCubeTexture(faces.data()));
                }
                assert(result && result->index == 0 && set.isTextureAlive(*result) && buffers.size() == 1);
                const auto image = set.slotImageView(result->index);
                assert(views.contains(image));
                assert(set.flushUploads());
                assert(buffers.empty() && commands.empty() && fences.empty());
                retirement.beginFrame(20);
                assert(set.removeTexture(*result) && !set.isTextureAlive(*result));
                retirement.collect(19);
                assert(views.contains(image));
                retirement.collect(20);
                assert(!views.contains(image) && images.size() == baseline_images);
                assert(views.size() == baseline_views && samplers.size() == baseline_samplers);
            }
        }
        // Every synchronous command failure keeps the original VkResult and releases staging only after safety.
        for (const auto boundary :
             {EFailure::ALLOCATE, EFailure::BEGIN, EFailure::END, EFailure::FENCE, EFailure::SUBMIT, EFailure::WAIT})
        {
            failure = EFailure::NONE;
            auto candidate = BindlessCombinedSet::create(info);
            assert(candidate);
            auto result = (*candidate)->addTexture(*texture);
            assert(result && buffers.size() == 1);
            failure = boundary;
            auto completed = (*candidate)->flushUploads();
            assert(!completed && isError<err::device::VulkanCallFailed>(completed.error()));
            const auto expected =
                boundary == EFailure::WAIT ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_OUT_OF_DEVICE_MEMORY;
            assert(completed.error().args[0] == encodeVkResult(expected));
            assert(commands.empty() && fences.empty());
            candidate->reset();
            assert(buffers.empty() && images.empty() && views.empty() && samplers.empty());
        }
        failure = EFailure::NONE;
        {
            auto candidate = BindlessCombinedSet::create(info);
            assert(candidate);
            auto& set = **candidate;
            const auto slot = set.addPersistentTexture(1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
            assert(slot && set.flushUploads());
            const auto view = set.slotImageView(slot->index);
            std::vector<BindlessCombinedSet::RegionUpdate> regions(lux::rdesc::kTextureMaxMipCount + 1);
            for (auto& region : regions)
            {
                region.width = region.height = 1;
            }
            failure = EFailure::BUFFER;
            skip_rejections = 1;
            assert(!set.updateTextureRegions(*slot, regions, pixels, 4));
            assert(skip_rejections == 0 && buffers.empty());
            assert(set.isTextureAlive(*slot) && set.slotImageView(slot->index) == view);
            failure = EFailure::NONE;
            assert(set.flushUploads() && buffers.empty());
            assert(set.updateTextureRegions(*slot, regions, pixels, 4));
            assert(buffers.size() == 2 && set.flushUploads() && buffers.empty());
        }
        {
            auto candidate = BindlessCombinedSet::create(info);
            assert(candidate);
            auto& set = **candidate;
            const auto slot = set.allocateSlotDeferred();
            assert(slot.isValid());
            auto install = [&](bool replace)
            {
                VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                image_info.imageType = VK_IMAGE_TYPE_2D;
                image_info.extent = {1, 1, 1};
                image_info.mipLevels = image_info.arrayLayers = 1;
                image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
                image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
                image_info.samples = VK_SAMPLE_COUNT_1_BIT;
                VmaAllocationCreateInfo allocation_info{};
                allocation_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
                auto image = VmaImage::create(device.vmaAllocator(), image_info, allocation_info);
                assert(image);
                VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                view_info.image = image->image();
                view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                view_info.format = image_info.format;
                view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                auto view = ImageViewOwner::create(device.logicalDevice(), view_info);
                assert(view);
                VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                auto sampler = SamplerOwner::create(device.logicalDevice(), sampler_info);
                assert(sampler);
                const auto native_image = image->release();
                const auto native_view = view->release();
                const auto native_sampler = sampler->release();
                if (replace)
                {
                    set.replaceTransferredTexture(
                        slot.index,
                        native_image.image,
                        native_image.allocation,
                        native_view,
                        native_sampler,
                        image_info.format,
                        1,
                        1,
                        1,
                        1
                    );
                }
                else
                {
                    set.finalizeTransferredTexture(
                        slot.index,
                        native_image.image,
                        native_image.allocation,
                        native_view,
                        native_sampler,
                        image_info.format,
                        1,
                        1,
                        1,
                        1
                    );
                }
                return native_view;
            };
            const auto previous = install(false);
            retirement.beginFrame(30);
            const auto current = install(true);
            assert(current != previous && set.slotImageView(slot.index) == current && set.isTextureAlive(slot));
            retirement.collect(29);
            assert(views.contains(previous) && views.contains(current));
            retirement.collect(30);
            assert(!views.contains(previous) && views.contains(current));
            retirement.beginFrame(31);
            assert(set.removeTexture(slot));
            retirement.collect(30);
            assert(views.contains(current));
            retirement.collect(31);
            assert(!views.contains(current));
        }
        std::puts("texture candidates: 2D/persistent/cube native rollback, retry, upload and serial retirement PASS");
        if (argc > 1 && std::string_view(argv[1]) == "--texture-upload")
        {
            return 0;
        }
    }
    for (unsigned invalid = 0; invalid < 5; ++invalid)
    {
        auto bad = info;
        switch (invalid)
        {
        case 0:
            bad.resource_context = nullptr;
            break;
        case 1:
            bad.deferred_queue = nullptr;
            break;
        case 2:
            bad.descriptor_set_layout = VK_NULL_HANDLE;
            break;
        case 3:
            bad.layout_max_capacity = 0;
            break;
        case 4:
            bad.frames_in_flight = 0;
            break;
        }
        auto rejected = BindlessCombinedSet::create(bad);
        assert(!rejected && isError<err::memory::InvalidBindlessConfiguration>(rejected.error()));
        assert(pools.size() == baseline_pools && sets.size() == baseline_sets && images.empty());
    }
    {
        auto owner = BindlessCombinedSet::create(info);
        assert(owner);
        auto& set = **owner;
        const auto original = set.descriptorSet();
        std::vector<SlotHandle> slots;
        for (unsigned i = 0; i < 8; ++i)
        {
            const auto slot = set.allocateSlotDeferred();
            assert(slot.isValid() && set.isTextureAlive(slot));
            slots.push_back(slot);
        }
        failure = EFailure::SET;
        const auto rejected = set.reserve(9);
        assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
        assert(set.descriptorSet() == original && set.capacity() == 8 && set.count() == 8);
        assert(!set.allocateSlotDeferred().isValid());
        for (const auto slot : slots)
        {
            assert(set.isTextureAlive(slot));
        }
        failure = EFailure::NONE;
        retirement.beginFrame(9);
        copied_descriptors = 0;
        const auto ninth = set.allocateSlotDeferred();
        assert(ninth.isValid() && set.capacity() == 16 && set.descriptorSet() != original);
        std::printf("live pending texture descriptors copied on growth=%u expected=8\n", copied_descriptors);
        std::fflush(stdout);
        assert(copied_descriptors == 8);
        assert(retirement.pendingCount() == 1 && sets.contains(original));
        retirement.collect(8);
        assert(sets.contains(original));
        retirement.collect(9);
        assert(!sets.contains(original));
        assert(set.removeTexture(slots.front()));
        set.recycleCompletedSlots(8);
        const auto tenth = set.allocateSlotDeferred();
        assert(tenth.index != slots.front().index);
        set.recycleCompletedSlots(9);
        const auto replacement = set.allocateSlotDeferred();
        assert(replacement.index == slots.front().index && replacement.gen != slots.front().gen);
    }
    assert(pools.size() == baseline_pools && sets.size() == baseline_sets);
    assert(images.empty() && views.empty() && samplers.empty());
    {
        VkDescriptorPoolSize size{
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            layouts.bindless2DCount() + layouts.bindlessCubeCount()
        };
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.flags =
            VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &size;
        auto pool = DescriptorPoolOwner::create(device.logicalDevice(), pool_info);
        assert(pool);
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = pool->get();
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &info.descriptor_set_layout;
        VkDescriptorSet borrowed{};
        assert(allocateSets(device.logicalDevice(), &allocation, &borrowed) == VK_SUCCESS);
        info.external_pool = pool->get();
        auto missing = BindlessCombinedSet::create(info);
        assert(!missing && isError<err::memory::InvalidBindlessConfiguration>(missing.error()));
        info.external_set = borrowed;
        failure = EFailure::VIEW;
        auto rejected = BindlessCombinedSet::create(info);
        assert(!rejected && pools.contains(pool->get()) && sets.contains(borrowed));
        assert(images.empty() && views.empty() && samplers.empty());
        failure = EFailure::NONE;
        {
            auto owner = BindlessCombinedSet::create(info);
            assert(owner && (*owner)->descriptorSet() == borrowed);
        }
        assert(pools.contains(pool->get()) && sets.contains(borrowed));
    }
    assert(pools.size() == baseline_pools && sets.size() == baseline_sets);
    assert(images.empty() && views.empty() && samplers.empty());
    assert(commands.empty() && fences.empty() && retirement.pendingCount() == 0);
    std::puts("Bindless mandatory backing: native prefixes, external borrow, growth and retirement PASS");
}
