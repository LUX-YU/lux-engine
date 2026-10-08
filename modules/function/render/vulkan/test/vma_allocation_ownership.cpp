#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <lux/engine/render/gpu/VulkanContext.hpp>

#include <cassert>
#include <map>
#include <type_traits>

namespace
{
    struct AllocationOrigin
    {
        VmaAllocator allocator{};
        VmaAllocation allocation{};
    };

    std::map<VkBuffer, AllocationOrigin> buffers;
    std::map<VkImage, AllocationOrigin> images;
    bool reject_buffer{}, reject_image{};

    VkResult createBuffer(
        VmaAllocator allocator,
        const VkBufferCreateInfo* info,
        const VmaAllocationCreateInfo* allocation_info,
        VkBuffer* buffer,
        VmaAllocation* allocation,
        VmaAllocationInfo* result_info
    )
    {
        *buffer = VK_NULL_HANDLE;
        *allocation = nullptr;
        if (reject_buffer)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreateBuffer(allocator, info, allocation_info, buffer, allocation, result_info);
        if (result == VK_SUCCESS)
        {
            assert(buffers.emplace(*buffer, AllocationOrigin{allocator, *allocation}).second);
        }
        return result;
    }

    void destroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation)
    {
        const auto found = buffers.find(buffer);
        assert(found != buffers.end());
        assert(found->second.allocator == allocator && found->second.allocation == allocation);
        buffers.erase(found);
        vmaDestroyBuffer(allocator, buffer, allocation);
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
        if (reject_image)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreateImage(allocator, info, allocation_info, image, allocation, result_info);
        if (result == VK_SUCCESS)
        {
            assert(images.emplace(*image, AllocationOrigin{allocator, *allocation}).second);
        }
        return result;
    }

    void destroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation)
    {
        const auto found = images.find(image);
        assert(found != images.end());
        assert(found->second.allocator == allocator && found->second.allocation == allocation);
        images.erase(found);
        vmaDestroyImage(allocator, image, allocation);
    }
} // namespace

// Keep the existing owners and exercise their actual implementations, including failure
// unwinding. The native allocator endpoints track real allocations from two actual devices.
// clang-format off
#define vmaCreateBuffer createBuffer
#define vmaDestroyBuffer destroyBuffer
#define vmaCreateImage createImage
#define vmaDestroyImage destroyImage
#include "../src/gpu/memory/VmaTypes.cpp"
#include "../src/gpu/memory/StagingBuffer.cpp"
#undef vmaDestroyImage
#undef vmaCreateImage
#undef vmaDestroyBuffer
#undef vmaCreateBuffer
#include "../src/gpu/VulkanContext.cpp"
// clang-format on

int main()
{
    using namespace lux::render;
    static_assert(!std::is_copy_constructible_v<VmaBuffer>);
    static_assert(!std::is_copy_constructible_v<VmaImage>);
    static_assert(!std::is_copy_constructible_v<StagingBuffer>);
    static_assert(std::is_nothrow_move_assignable_v<VmaBuffer>);
    static_assert(std::is_nothrow_move_assignable_v<VmaImage>);
    static_assert(std::is_nothrow_move_assignable_v<StagingBuffer>);

    auto instance_owner = InstanceContext::create({});
    assert(instance_owner);
    auto& instance = **instance_owner;
    DeviceContext first_device(instance), second_device(instance);
    assert(first_device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
    assert(second_device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
    const auto first_allocator = first_device.vmaAllocator();
    const auto second_allocator = second_device.vmaAllocator();

    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = 256;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo buffer_allocation{};
    buffer_allocation.usage = VMA_MEMORY_USAGE_AUTO;
    buffer_allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    {
        reject_buffer = true;
        const auto failed = VmaBuffer::create(first_allocator, buffer_info, buffer_allocation);
        assert(!failed && isError<err::device::VulkanCallFailed>(failed.error()) && buffers.empty());
        reject_buffer = false;
        auto first = VmaBuffer::create(first_allocator, buffer_info, buffer_allocation);
        auto second = VmaBuffer::create(second_allocator, buffer_info, buffer_allocation);
        assert(first && second && buffers.size() == 2);
        *first = std::move(*second);
        assert(!*second && buffers.size() == 1);
        VmaBuffer moved(std::move(*first));
        assert(!*first && moved.map());
        moved.flush();
        moved.unmap();
        moved.reset();
        moved.reset();
    }
    assert(buffers.empty());

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent = {4, 4, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    VmaAllocationCreateInfo image_allocation{};
    image_allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    {
        reject_image = true;
        const auto failed = VmaImage::create(first_allocator, image_info, image_allocation);
        assert(!failed && isError<err::device::VulkanCallFailed>(failed.error()) && images.empty());
        reject_image = false;
        auto first = VmaImage::create(first_allocator, image_info, image_allocation);
        auto second = VmaImage::create(second_allocator, image_info, image_allocation);
        assert(first && second && images.size() == 2);
        *first = std::move(*second);
        assert(!*second && images.size() == 1);
        VmaImage moved(std::move(*first));
        assert(!*first && moved);
    }
    assert(images.empty());

    {
        VkBuffer first_buffer{}, second_buffer{};
        VmaAllocation first_allocation{}, second_allocation{};
        assert(
            createBuffer(
                first_allocator,
                &buffer_info,
                &buffer_allocation,
                &first_buffer,
                &first_allocation,
                nullptr
            ) == VK_SUCCESS
        );
        StagingBuffer first(first_allocator, first_buffer, first_allocation);
        assert(
            createBuffer(
                second_allocator,
                &buffer_info,
                &buffer_allocation,
                &second_buffer,
                &second_allocation,
                nullptr
            ) == VK_SUCCESS
        );
        StagingBuffer second(second_allocator, second_buffer, second_allocation);
        first = std::move(second);
        assert(!second.valid() && buffers.size() == 1);
        StagingBuffer moved(std::move(first));
        assert(!first.valid() && moved.buffer() == second_buffer);
    }
    assert(buffers.empty());
    for (const auto allocator : {first_allocator, second_allocator})
    {
        VmaTotalStatistics statistics{};
        vmaCalculateStatistics(allocator, &statistics);
        assert(statistics.total.statistics.allocationCount == 0);
    }
}
