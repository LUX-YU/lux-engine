#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <lux/engine/render/gpu/VulkanContext.hpp>

#include <cassert>
#include <cstdio>
#include <map>
#include <type_traits>

namespace
{
    enum class EFailure
    {
        NONE,
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
    EFailure failure{};
    unsigned rejections{}, idle_calls{};
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
#include "../src/gpu/memory/VmaTypes.cpp"
#define vkDeviceWaitIdle trackedDeviceWaitIdle
#include "../src/gpu/VulkanContext.cpp"
#undef vkDeviceWaitIdle
#define vkAllocateCommandBuffers allocate
#define vkFreeCommandBuffers freeCommands
#define vkBeginCommandBuffer begin
#define vkEndCommandBuffer end
#define vkCreateFence createFence
#define vkDestroyFence destroyFence
#define vkQueueSubmit submit
#define vkWaitForFences wait
#include "../src/gpu/pipeline/GeneralDescriptorSetLayout.cpp"
#include "../src/resources/descriptor/BindlessCombinedSet.cpp"
#undef vkWaitForFences
#undef vkQueueSubmit
#undef vkDestroyFence
#undef vkCreateFence
#undef vkEndCommandBuffer
#undef vkBeginCommandBuffer
#undef vkFreeCommandBuffers
#undef vkAllocateCommandBuffers
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
    ResourceContext resources(device);
    assert(resources.init());
    auto layout_owner = GeneralDescriptorSetLayout::create(device);
    assert(layout_owner);
    auto& layouts = **layout_owner;

    {
        DeviceContext other_device(instance);
        assert(other_device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
        ResourceContext other_resources(other_device);
        assert(other_resources.init());
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

    for (auto boundary :
         {EFailure::NONE,
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
            BCInitInfo info{};
            info.resource_context = &resources;
            info.descriptor_set_layout = layouts.getLayout(TGetBindingSet<ETextureSetBindings>::value);
            info.layout_max_capacity = layouts.bindless2DCount();
            info.initial_capacity = 8;
            BindlessCombinedSet set;
            assert(set.init(info));
        }
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
}
