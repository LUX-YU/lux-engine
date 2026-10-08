#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/graph/RenderGraphCompiler.hpp>

#include <cassert>
#include <map>
#include <type_traits>

namespace
{
    struct ImageOrigin
    {
        VmaAllocator allocator{};
        VmaAllocation allocation{};
        VmaPool pool{};
    };

    std::map<VmaPool, VmaAllocator> pools;
    std::map<VkImage, ImageOrigin> images;
    bool reject_pool{};
    unsigned image_attempts{}, reject_image_at{};

    VkResult createPool(VmaAllocator allocator, const VmaPoolCreateInfo* info, VmaPool* out)
    {
        *out = nullptr;
        if (reject_pool)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreatePool(allocator, info, out);
        if (result == VK_SUCCESS)
        {
            assert(pools.emplace(*out, allocator).second);
        }
        return result;
    }

    void destroyPool(VmaAllocator allocator, VmaPool pool)
    {
        const auto found = pools.find(pool);
        assert(found != pools.end() && found->second == allocator);
        for (const auto& [image, origin] : images)
        {
            assert(origin.pool != pool);
        }
        VmaStatistics stats{};
        vmaGetPoolStatistics(allocator, pool, &stats);
        assert(stats.allocationCount == 0);
        pools.erase(found);
        vmaDestroyPool(allocator, pool);
    }

    VkResult trackedCreateImage(
        VmaAllocator allocator,
        const VkImageCreateInfo* info,
        const VmaAllocationCreateInfo* allocation_info,
        VkImage* image,
        VmaAllocation* allocation,
        VmaAllocationInfo* output
    )
    {
        *image = VK_NULL_HANDLE;
        *allocation = nullptr;
        if (++image_attempts == reject_image_at)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreateImage(allocator, info, allocation_info, image, allocation, output);
        if (result == VK_SUCCESS)
        {
            assert(images.emplace(*image, ImageOrigin{allocator, *allocation, allocation_info->pool}).second);
        }
        return result;
    }

    void trackedDestroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation)
    {
        const auto found = images.find(image);
        assert(found != images.end());
        assert(found->second.allocator == allocator && found->second.allocation == allocation);
        images.erase(found);
        vmaDestroyImage(allocator, image, allocation);
    }
} // namespace

// Actual allocator, custom pool and graph resource allocation paths. No private test access.
// clang-format off
#define vmaCreatePool createPool
#define vmaDestroyPool destroyPool
#define vmaCreateImage trackedCreateImage
#define vmaDestroyImage trackedDestroyImage
#include "../src/gpu/memory/VmaTypes.cpp"
#include "../src/graph/RGVulkanResourceAllocator.cpp"
#undef vmaDestroyImage
#undef vmaCreateImage
#undef vmaDestroyPool
#undef vmaCreatePool
#include "../src/gpu/VulkanContext.cpp"
// clang-format on

int main()
{
    using namespace lux::render;
    static_assert(!std::is_copy_constructible_v<VmaPoolOwner>);
    static_assert(!std::is_copy_assignable_v<VmaPoolOwner>);
    static_assert(std::is_nothrow_move_constructible_v<VmaPoolOwner>);
    static_assert(std::is_nothrow_move_assignable_v<VmaPoolOwner>);
    static_assert(std::is_nothrow_destructible_v<VmaPoolOwner>);
    static_assert(!std::is_copy_constructible_v<RGVulkanResourceAllocator>);
    static_assert(!std::is_move_constructible_v<RGVulkanResourceAllocator>);

    auto instance_owner = InstanceContext::create({});
    assert(instance_owner);
    auto& instance = **instance_owner;
    DeviceContext device(instance), other_device(instance);
    assert(device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
    assert(other_device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
    auto resources_owner = ResourceContext::create(device);
    assert(resources_owner);
    auto& resources = **resources_owner;
    {
        VmaPoolCreateInfo info{};
        reject_pool = true;
        const auto rejected = VmaPoolOwner::create(device.vmaAllocator(), info);
        assert(!rejected && rejected.error() == VK_ERROR_OUT_OF_DEVICE_MEMORY && pools.empty());
        reject_pool = false;
        auto first = VmaPoolOwner::create(device.vmaAllocator(), info);
        auto second = VmaPoolOwner::create(other_device.vmaAllocator(), info);
        assert(first && second && pools.size() == 2);
        *first = std::move(*second);
        assert(!*second && pools.size() == 1);
        VmaPoolOwner moved(std::move(*first));
        assert(!*first && moved);
    }
    assert(pools.empty());

    RGGraphDescription graph;
    auto& resource = graph.resources.emplace_back();
    resource.lifetime = ERGResourceLifetime::TRANSIENT;
    auto texture = RGTextureDescription::Absolute(8, 8);
    texture.usage = static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::SAMPLED);
    resource.desc = texture;
    {
        RGVulkanResourceAllocator allocator(resources);
        auto first = allocator.allocate(graph, {}, {}, {8, 8}, 2);
        assert(first && images.size() == 2 && pools.size() == 1);
        const auto attempts = image_attempts;
        allocator.deallocateToPool(*first, graph);
        auto reused = allocator.allocate(graph, {}, {}, {8, 8}, 2);
        assert(reused && image_attempts == attempts && images.size() == 2);
        allocator.deallocateToPool(*reused, graph);
        allocator.releasePool();
        assert(images.empty() && pools.empty());
        allocator.releasePool();
    }

    // A rejected custom pool preserves the existing default-allocation fallback.
    reject_pool = true;
    {
        RGVulkanResourceAllocator allocator(resources);
        auto fallback = allocator.allocate(graph, {}, {}, {8, 8}, 2);
        assert(fallback && pools.empty() && images.size() == 2);
        for (const auto& [image, origin] : images)
        {
            assert(origin.pool == nullptr);
        }
        allocator.deallocateToPool(*fallback, graph);
    }
    assert(images.empty() && pools.empty());
    reject_pool = false;

    for (unsigned boundary = 1; boundary <= 2; ++boundary)
    {
        image_attempts = 0;
        reject_image_at = boundary;
        {
            RGVulkanResourceAllocator allocator(resources);
            const auto rejected = allocator.allocate(graph, {}, {}, {8, 8}, 2);
            assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
            assert(image_attempts == boundary && images.empty() && pools.size() == 1);
        }
        assert(pools.empty());
    }
}
