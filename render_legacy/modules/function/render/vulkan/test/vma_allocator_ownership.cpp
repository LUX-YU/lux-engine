#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <cassert>
#include <cstdio>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace
{
    std::unordered_map<VmaAllocator, VmaAllocatorInfo> live;
    bool reject_creation{};
    unsigned created{}, destroyed{};
    unsigned queries{}, fail_query{}, incomplete_fills{}, device_created{}, device_destroyed{};
    bool reject_device{};
    std::unordered_map<VkDevice, const VkAllocationCallbacks*> devices;

    VkResult VKAPI_CALL createDevice(
        VkPhysicalDevice physical,
        const VkDeviceCreateInfo* info,
        const VkAllocationCallbacks* allocator,
        VkDevice* output
    )
    {
        std::set<std::string> extensions;
        for (uint32_t index = 0; index < info->enabledExtensionCount; ++index)
        {
            assert(extensions.insert(info->ppEnabledExtensionNames[index]).second);
        }
        std::set<uint32_t> families;
        for (uint32_t index = 0; index < info->queueCreateInfoCount; ++index)
        {
            assert(families.insert(info->pQueueCreateInfos[index].queueFamilyIndex).second);
        }
        if (reject_device)
        {
            return VK_ERROR_DEVICE_LOST;
        }
        const auto result = vkCreateDevice(physical, info, allocator, output);
        if (result == VK_SUCCESS)
        {
            assert(devices.emplace(*output, allocator).second);
            ++device_created;
        }
        return result;
    }

    void VKAPI_CALL destroyDevice(VkDevice device, const VkAllocationCallbacks* allocator)
    {
        assert(devices.contains(device) && devices.at(device) == allocator);
        for (const auto& [handle, info] : live)
        {
            assert(info.device != device); // VMA must have released before its native device.
        }
        devices.erase(device);
        ++device_destroyed;
        vkDestroyDevice(device, allocator);
    }

    VkResult VKAPI_CALL
    deviceExtensions(VkPhysicalDevice device, const char* layer, uint32_t* count, VkExtensionProperties* values)
    {
        if (++queries == fail_query)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        if (values && incomplete_fills)
        {
            --incomplete_fills;
            return VK_INCOMPLETE;
        }
        return vkEnumerateDeviceExtensionProperties(device, layer, count, values);
    }

    VkResult createAllocator(const VmaAllocatorCreateInfo* info, VmaAllocator* output)
    {
        assert(!(info->flags & VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT));
        assert(devices.contains(info->device));
        *output = nullptr;
        if (reject_creation)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreateAllocator(info, output);
        if (result == VK_SUCCESS)
        {
            VmaAllocatorInfo actual{};
            vmaGetAllocatorInfo(*output, &actual);
            assert(actual.device == info->device && actual.instance == info->instance);
            assert(actual.physicalDevice == info->physicalDevice);
            assert(live.emplace(*output, actual).second);
            ++created;
        }
        return result;
    }

    void destroyAllocator(VmaAllocator allocator)
    {
        const auto found = live.find(allocator);
        assert(found != live.end());
        VmaAllocatorInfo actual{};
        vmaGetAllocatorInfo(allocator, &actual);
        assert(actual.device == found->second.device && actual.instance == found->second.instance);
        VmaTotalStatistics stats{};
        vmaCalculateStatistics(allocator, &stats);
        assert(stats.total.statistics.allocationCount == 0);
        live.erase(found);
        vmaDestroyAllocator(allocator);
        ++destroyed;
    }
} // namespace

// Exercise actual allocator and DeviceContext implementations. Native discovery,
// device and allocator boundaries forward successful calls to real Vulkan/VMA.
// clang-format off
#define vkCreateDevice createDevice
#define vkDestroyDevice destroyDevice
#define vkEnumerateDeviceExtensionProperties deviceExtensions
#define vmaCreateAllocator createAllocator
#define vmaDestroyAllocator destroyAllocator
#include "../src/gpu/memory/VmaTypes.cpp"
#undef vmaDestroyAllocator
#undef vmaCreateAllocator
#include "../src/gpu/VulkanContext.cpp"
#undef vkEnumerateDeviceExtensionProperties
#undef vkDestroyDevice
#undef vkCreateDevice
// clang-format on

int main()
{
    using namespace lux::render;
    static_assert(!std::is_copy_constructible_v<VmaAllocatorOwner>);
    static_assert(!std::is_copy_assignable_v<VmaAllocatorOwner>);
    static_assert(std::is_nothrow_move_constructible_v<VmaAllocatorOwner>);
    static_assert(std::is_nothrow_move_assignable_v<VmaAllocatorOwner>);
    static_assert(std::is_nothrow_destructible_v<VmaAllocatorOwner>);

    auto instance_owner = InstanceContext::create({});
    assert(instance_owner);
    auto& instance = **instance_owner;
    static_assert(!std::is_constructible_v<DeviceContext, InstanceContext&>);
    static_assert(!std::is_copy_constructible_v<DeviceContext>);
    static_assert(!std::is_move_constructible_v<DeviceContext>);
    {
        const auto rejected = DeviceContext::create(instance, static_cast<EPhysicalDeviceSelectionPolicy>(99));
        assert(!rejected && isError<err::internal::InvalidArgument>(rejected.error()));
        assert(queries == 0 && device_created == 0);
    }
    for (unsigned fault = 1; fault <= 2; ++fault)
    {
        queries = 0;
        fail_query = fault;
        const auto result = DeviceContext::create(instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
        assert(!result && isError<err::device::VulkanCallFailed>(result.error()));
        assert(result.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_HOST_MEMORY));
        assert(live.empty() && devices.empty() && device_created == 0);
    }
    fail_query = 0;
    incomplete_fills = 3;
    {
        const auto result = DeviceContext::create(instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
        assert(!result && isError<err::device::VulkanCallFailed>(result.error()));
        assert(result.error().args[0] == encodeVkResult(VK_INCOMPLETE));
        assert(live.empty() && devices.empty() && device_created == 0);
    }
    reject_device = true;
    {
        const auto result = DeviceContext::create(instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
        assert(!result && isError<err::device::VulkanCallFailed>(result.error()));
        assert(result.error().args[0] == encodeVkResult(VK_ERROR_DEVICE_LOST));
        assert(live.empty() && devices.empty() && device_created == 0);
    }
    reject_device = false;
    reject_creation = true;
    {
        const auto result = DeviceContext::create(instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
        assert(!result && isError<err::device::VulkanCallFailed>(result.error()));
        assert(result.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        assert(live.empty() && devices.empty());
        assert(device_created == 1 && device_destroyed == 1);
    }
    assert(created == 0 && destroyed == 0);

    reject_creation = false;
    incomplete_fills = 1;
    {
        auto first_device_owner =
            DeviceContext::create(instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
        assert(first_device_owner);
        auto& first_device = **first_device_owner;
        auto second_device_owner =
            DeviceContext::create(instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
        assert(second_device_owner);
        auto& second_device = **second_device_owner;
        assert(live.size() == 2);
        assert(incomplete_fills == 0 && devices.size() == 2);
        assert(first_device.caps().synchronization2 && first_device.caps().dynamic_rendering);
        assert(first_device.graphicsQueue().handle());
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = first_device.graphicsQueueFamilyIndex();
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo native_info{};
        native_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        native_info.queueCreateInfoCount = 1;
        native_info.pQueueCreateInfos = &queue;
        {
            using lux::gapi::vk::LogicalDevice;
            static_assert(!std::is_copy_constructible_v<LogicalDevice>);
            static_assert(std::is_nothrow_move_constructible_v<LogicalDevice>);
            auto first = LogicalDevice::create(first_device.physicalDevice(), native_info);
            auto second = LogicalDevice::create(first_device.physicalDevice(), native_info);
            assert(first && second && devices.size() == 4);
            const auto kept = second->handle();
            *first = std::move(*second);
            assert(first->handle() == kept && !second->handle() && devices.size() == 3);
            *first = std::move(*first);
            LogicalDevice moved(std::move(*first));
            assert(moved.handle() == kept && !first->handle());
            moved.reset();
            moved.reset();
            assert(devices.size() == 2);
        }

        VmaAllocatorCreateInfo info{};
        info.instance = instance.instance();
        info.physicalDevice = first_device.physicalDevice();
        info.device = first_device.logicalDevice();
        info.vulkanApiVersion = VK_API_VERSION_1_3;
        reject_creation = true;
        const auto rejected = VmaAllocatorOwner::create(info);
        assert(!rejected && rejected.error() == VK_ERROR_OUT_OF_DEVICE_MEMORY && live.size() == 2);
        reject_creation = false;

        auto first = VmaAllocatorOwner::create(info);
        info.physicalDevice = second_device.physicalDevice();
        info.device = second_device.logicalDevice();
        auto second = VmaAllocatorOwner::create(info);
        assert(first && second && live.size() == 4);
        const auto second_handle = second->get();

        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = 256;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
        allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        auto buffer = VmaBuffer::create(second_handle, buffer_info, allocation_info);
        assert(buffer);
        assert(buffer->map());
        buffer->unmap();

        *first = std::move(*second);
        assert(!*second && first->get() == second_handle && live.size() == 3);
        *first = std::move(*first);
        assert(first->get() == second_handle && live.size() == 3);
        VmaAllocatorOwner moved(std::move(*first));
        assert(!*first && moved.get() == second_handle);
        buffer->reset();
        moved.reset();
        moved.reset();
        assert(!moved && live.size() == 2);
    }
    assert(live.empty() && created == destroyed);
    assert(devices.empty() && device_created == device_destroyed);
    std::puts("Device construction: discovery/native/VMA rejection, exact errors, complete retry, leaf move, "
              "VMA-before-device release PASS");
}
