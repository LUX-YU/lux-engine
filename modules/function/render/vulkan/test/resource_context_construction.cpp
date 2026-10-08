#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <type_traits>
#include <vector>

namespace
{
    struct Allocation
    {
        void* base;
        std::size_t size;
    };

    std::atomic_size_t allocation_count{};

    void* VKAPI_PTR allocate(void*, std::size_t size, std::size_t alignment, VkSystemAllocationScope)
    {
        alignment = std::max(alignment, alignof(Allocation));
        const auto bytes = sizeof(Allocation) + size + alignment - 1;
        void* base = std::malloc(bytes);
        if (!base)
        {
            return nullptr;
        }
        void* result = static_cast<std::byte*>(base) + sizeof(Allocation);
        auto space = bytes - sizeof(Allocation);
        const auto aligned = std::align(alignment, size, result, space);
        assert(aligned);
        auto* header = static_cast<Allocation*>(result) - 1;
        *header = {base, size};
        ++allocation_count;
        return result;
    }

    void VKAPI_PTR freeAllocation(void*, void* value)
    {
        if (value)
        {
            std::free((static_cast<Allocation*>(value) - 1)->base);
            --allocation_count;
        }
    }

    void* VKAPI_PTR
    reallocate(void* user, void* original, std::size_t size, std::size_t alignment, VkSystemAllocationScope scope)
    {
        if (!size)
        {
            freeAllocation(user, original);
            return nullptr;
        }
        void* result = allocate(user, size, alignment, scope);
        if (result && original)
        {
            std::memcpy(result, original, std::min(size, (static_cast<Allocation*>(original) - 1)->size));
            freeAllocation(user, original);
        }
        return result;
    }

    struct Origin
    {
        VkDevice device;
        const VkAllocationCallbacks* allocator;
    };

    std::map<VkDescriptorPool, Origin> descriptors;
    std::map<VkCommandPool, Origin> commands;
    std::uintptr_t next_handle{100};
    unsigned attempts{}, fail_at{}, callback_mismatches{};
    std::vector<std::uintptr_t> live_order;
    std::vector<std::uint32_t> queue_families;
    std::uint32_t expected_max_sets{256};

    VkResult createDescriptors(
        VkDevice device,
        const VkDescriptorPoolCreateInfo* info,
        const VkAllocationCallbacks* allocator,
        VkDescriptorPool* output
    )
    {
        assert(info->poolSizeCount == 11 && info->maxSets == expected_max_sets);
        assert(
            info->flags ==
            (VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)
        );
        assert(allocator);
        *output = VK_NULL_HANDLE;
        if (++attempts == fail_at)
        {
            return VK_ERROR_DEVICE_LOST;
        }
        *output = reinterpret_cast<VkDescriptorPool>(++next_handle);
        assert(descriptors.emplace(*output, Origin{device, allocator}).second);
        live_order.push_back(next_handle);
        return VK_SUCCESS;
    }

    void destroyDescriptors(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks* allocator)
    {
        const auto found = descriptors.find(pool);
        assert(found != descriptors.end() && found->second.device == device);
        callback_mismatches += found->second.allocator != allocator;
        assert(live_order.back() == reinterpret_cast<std::uintptr_t>(pool));
        live_order.pop_back();
        descriptors.erase(found);
    }

    VkResult createCommands(
        VkDevice device,
        const VkCommandPoolCreateInfo* info,
        const VkAllocationCallbacks* allocator,
        VkCommandPool* output
    )
    {
        assert(info->flags == VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
        *output = VK_NULL_HANDLE;
        if (++attempts == fail_at)
        {
            return VK_ERROR_DEVICE_LOST;
        }
        *output = reinterpret_cast<VkCommandPool>(++next_handle);
        assert(commands.emplace(*output, Origin{device, allocator}).second);
        live_order.push_back(next_handle);
        queue_families.push_back(info->queueFamilyIndex);
        return VK_SUCCESS;
    }

    void destroyCommands(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* allocator)
    {
        const auto found = commands.find(pool);
        assert(found != commands.end() && found->second.device == device);
        callback_mismatches += found->second.allocator != allocator;
        assert(live_order.back() == reinterpret_cast<std::uintptr_t>(pool));
        live_order.pop_back();
        commands.erase(found);
    }
} // namespace

// Instrument the exact native pool boundary of the real context implementation.
// Instance/device/VMA still use their real native implementations and valid allocation callbacks.
// clang-format off
#define vkCreateDescriptorPool createDescriptors
#define vkDestroyDescriptorPool destroyDescriptors
#define vkCreateCommandPool createCommands
#define vkDestroyCommandPool destroyCommands
#include "../src/gpu/memory/VmaTypes.cpp"
#include "../src/gpu/VulkanContext.cpp"
#undef vkDestroyCommandPool
#undef vkCreateCommandPool
#undef vkDestroyDescriptorPool
#undef vkCreateDescriptorPool
// clang-format on

void checkServerStartupPrefix();

int main()
{
    using namespace lux::render;
    static_assert(!std::is_constructible_v<ResourceContext, DeviceContext&>);
    static_assert(!std::is_copy_constructible_v<ResourceContext>);
    static_assert(!std::is_move_constructible_v<ResourceContext>);
    static_assert(std::is_same_v<decltype(std::declval<const ResourceContext&>().commandPool()), VkCommandPool>);
    VkAllocationCallbacks allocator{};
    allocator.pfnAllocation = allocate;
    allocator.pfnReallocation = reallocate;
    allocator.pfnFree = freeAllocation;
    {
        auto instance_owner = InstanceContext::create({}, {}, &allocator);
        assert(instance_owner);
        auto& instance = **instance_owner;
        DeviceContext device(instance);
        assert(device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
        for (unsigned failure = 1; failure <= 4; ++failure)
        {
            attempts = 0;
            fail_at = failure;
            auto result = ResourceContext::create(device);
            assert(!result && isError<err::device::VulkanCallFailed>(result.error()));
            assert(result.error().args[0] == encodeVkResult(VK_ERROR_DEVICE_LOST));
            assert(attempts == failure);
            // Complete cleanup precedes failure publication, not a later semantic-object destructor.
            assert(descriptors.empty() && commands.empty() && live_order.empty());
            assert(callback_mismatches == 0);
        }
        fail_at = 0;
        attempts = 0;
        queue_families.clear();
        DescriptorPoolConfig config;
        config.max_sets = expected_max_sets = 37;
        {
            auto resources = ResourceContext::create(device, config);
            assert(resources && attempts == 4);
            assert(descriptors.contains((*resources)->descriptorPool()));
            assert(commands.contains((*resources)->commandPool()));
            assert(commands.contains((*resources)->computeCommandPool()));
            assert(commands.contains((*resources)->transferCommandPool()));
            const std::vector<std::uint32_t> expected_families{
                device.graphicsQueueFamilyIndex(),
                device.asyncComputeQueueFamilyIndex(),
                device.transferQueueFamilyIndex()
            };
            assert(queue_families == expected_families);
        }
        assert(commands.empty() && descriptors.empty() && live_order.empty());
        assert(callback_mismatches == 0);
    }
    assert(allocation_count == 0);
    checkServerStartupPrefix();
    std::puts("resource construction: four native faults, retry, complete borrows, reverse cleanup and allocators PASS"
    );
}
