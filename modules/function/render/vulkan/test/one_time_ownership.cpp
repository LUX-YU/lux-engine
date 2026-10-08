#include <lux/engine/gapi/vk/vk.hpp>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <type_traits>

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
        ALLOCATE,
        BEGIN,
        END,
        FENCE,
        SUBMIT,
        WAIT
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
    EFailure failure{};
    unsigned rejections{}, idle_calls{}, copied_descriptors{};
    bool submitted{};

    bool reject(EFailure boundary)
    {
        if (failure != boundary)
        {
            return false;
        }
        ++rejections;
        return true;
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
            return VK_ERROR_OUT_OF_POOL_MEMORY;
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

    void updateDescriptors(
        VkDevice device,
        std::uint32_t write_count,
        const VkWriteDescriptorSet* writes,
        std::uint32_t copy_count,
        const VkCopyDescriptorSet* copies
    )
    {
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

    VkResult trackedDeviceWaitIdle(VkDevice device)
    {
        ++idle_calls;
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
#include "../src/gpu/memory/VmaTypes.cpp"
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
#undef vmaCreateImage
#undef vmaDestroyImage
#undef vkCreateImageView
#undef vkDestroyImageView
#undef vkCreateSampler
#undef vkDestroySampler
// clang-format on

int main()
{
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
    DeferredDestroyQueue retirement;
    retirement.init(device.vmaAllocator(), device.logicalDevice());
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

    failure = EFailure::NONE;
    BindlessSetCreateInfo info{};
    info.resource_context = &resources;
    info.deferred_queue = &retirement;
    info.descriptor_set_layout = layouts.getLayout(TGetBindingSet<ETextureSetBindings>::value);
    info.layout_max_capacity = layouts.bindless2DCount();
    info.initial_capacity = 8;
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
