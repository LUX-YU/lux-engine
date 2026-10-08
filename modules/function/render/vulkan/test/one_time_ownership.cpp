#include <lux/engine/gapi/vk/vk.hpp>
#include <lux/engine/render/gpu/VmaFwd.hpp>

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
        BUFFER,
        MAPPED,
        FLUSH,
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
    std::map<VkBuffer, std::pair<VmaAllocator, VmaAllocation>> buffers;
    EFailure failure{};
    unsigned rejections{}, idle_calls{}, copied_descriptors{}, skip_rejections{};
    bool submitted{};

    bool reject(EFailure boundary)
    {
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
            if (mapped && reject(EFailure::MAPPED))
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

int main(int argc, char** argv)
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
