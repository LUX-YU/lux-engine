#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <lux/engine/render/gpu/VulkanContext.hpp>

#include <cassert>
#include <type_traits>
#include <unordered_map>

namespace
{
    std::unordered_map<VmaAllocator, VmaAllocatorInfo> live;
    bool reject_creation{};
    unsigned created{}, destroyed{};

    VkResult createAllocator(const VmaAllocatorCreateInfo* info, VmaAllocator* output)
    {
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

// Exercise actual allocator and DeviceContext implementations. Only native allocator
// creation/destruction are intercepted; real device and allocation backing remain in use.
// clang-format off
#define vmaCreateAllocator createAllocator
#define vmaDestroyAllocator destroyAllocator
#include "../src/gpu/memory/VmaTypes.cpp"
#undef vmaDestroyAllocator
#undef vmaCreateAllocator
#include "../src/gpu/VulkanContext.cpp"
// clang-format on

int main()
{
    using namespace lux::render;
    static_assert(!std::is_copy_constructible_v<VmaAllocatorOwner>);
    static_assert(!std::is_copy_assignable_v<VmaAllocatorOwner>);
    static_assert(std::is_nothrow_move_constructible_v<VmaAllocatorOwner>);
    static_assert(std::is_nothrow_move_assignable_v<VmaAllocatorOwner>);
    static_assert(std::is_nothrow_destructible_v<VmaAllocatorOwner>);

    InstanceContext instance({});
    reject_creation = true;
    {
        DeviceContext rejected(instance);
        const auto result = rejected.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
        assert(!result && isError<err::device::VulkanObjectCreationFailed>(result.error()));
        assert(rejected.vmaAllocator() == nullptr && live.empty());
    }
    assert(created == 0 && destroyed == 0);

    reject_creation = false;
    {
        DeviceContext first_device(instance), second_device(instance);
        assert(first_device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
        assert(second_device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
        assert(live.size() == 2);

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
}
