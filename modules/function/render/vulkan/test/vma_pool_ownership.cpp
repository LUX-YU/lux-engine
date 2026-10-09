#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/graph/RenderGraphCompiler.hpp>

#include <cassert>
#include <map>
#include <type_traits>

namespace
{
    struct AllocationOrigin
    {
        VmaAllocator allocator{};
        VmaAllocation allocation{};
        VmaPool pool{};
    };

    std::map<VmaPool, VmaAllocator> pools;
    std::map<VkImage, AllocationOrigin> images;
    std::map<VkBuffer, AllocationOrigin> buffers;
    bool reject_pool{};
    unsigned image_attempts{}, reject_image_at{};
    unsigned buffer_attempts{}, reject_buffer_at{};
    VkImage borrowed_image{};

    uint32_t borrowedImages(VkImage* output, uint32_t capacity)
    {
        assert(capacity > 0);
        output[0] = borrowed_image;
        return 1;
    }

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
        for (const auto& [buffer, origin] : buffers)
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
            assert(images.emplace(*image, AllocationOrigin{allocator, *allocation, allocation_info->pool}).second);
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

    VkResult trackedCreateBuffer(
        VmaAllocator allocator,
        const VkBufferCreateInfo* info,
        const VmaAllocationCreateInfo* allocation_info,
        VkBuffer* buffer,
        VmaAllocation* allocation,
        VmaAllocationInfo* output
    )
    {
        *buffer = VK_NULL_HANDLE;
        *allocation = nullptr;
        if (++buffer_attempts == reject_buffer_at)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreateBuffer(allocator, info, allocation_info, buffer, allocation, output);
        if (result == VK_SUCCESS)
        {
            assert(buffers.emplace(*buffer, AllocationOrigin{allocator, *allocation, allocation_info->pool}).second);
        }
        return result;
    }

    void trackedDestroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation)
    {
        const auto found = buffers.find(buffer);
        assert(found != buffers.end());
        assert(found->second.allocator == allocator && found->second.allocation == allocation);
        buffers.erase(found);
        vmaDestroyBuffer(allocator, buffer, allocation);
    }
} // namespace

// Actual allocator, custom pool and graph resource allocation paths. No private test access.
// clang-format off
#define vmaCreatePool createPool
#define vmaDestroyPool destroyPool
#define vmaCreateImage trackedCreateImage
#define vmaDestroyImage trackedDestroyImage
#define vmaCreateBuffer trackedCreateBuffer
#define vmaDestroyBuffer trackedDestroyBuffer
#include "../src/gpu/memory/VmaTypes.cpp"
#include "../src/graph/RGVulkanResourceAllocator.cpp"
#undef vmaDestroyBuffer
#undef vmaCreateBuffer
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
    auto device_owner = DeviceContext::create(instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
    assert(device_owner);
    auto& device = **device_owner;
    auto other_device_owner = DeviceContext::create(instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
    assert(other_device_owner);
    auto& other_device = **other_device_owner;
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

    reject_image_at = 0;
    RGResourceDescription buffer;
    buffer.type = ERGResourceType::BUFFER;
    buffer.lifetime = ERGResourceLifetime::TRANSIENT;
    buffer.desc = RGBufferDescription{
        1024,
        4,
        256,
        static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE),
        ERGMemoryUsage::GPU_ONLY
    };

    for (const auto& description : {resource, buffer})
    {
        RGGraphDescription ring;
        auto current = description;
        current.lifetime = ERGResourceLifetime::PING_PONG;
        current.ring_size = 3;
        current.ring_phase = 0;
        ring.resources.push_back(current);
        auto previous = current;
        previous.ring_phase = 1;
        previous.pingpong_peer = 0;
        ring.resources.push_back(previous);

        for (unsigned boundary = 1; boundary <= 3; ++boundary)
        {
            image_attempts = buffer_attempts = 0;
            reject_image_at = reject_buffer_at = boundary;
            RGVulkanResourceAllocator allocator(resources);
            const auto rejected = allocator.allocate(ring, {}, {}, {8, 8}, 2);
            assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
            assert(rejected.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
            assert(images.empty() && buffers.empty());
            const auto attempts = description.type == ERGResourceType::TEXTURE ? image_attempts : buffer_attempts;
            assert(attempts == boundary);
        }
        reject_image_at = reject_buffer_at = 0;
        for (const bool to_pool : {false, true})
        {
            RGVulkanResourceAllocator allocator(resources);
            auto table = allocator.allocate(ring, {}, {}, {8, 8}, 2);
            assert(table && images.size() + buffers.size() == 3);
            assert(table->at(0).physical_handles.size() == 3);
            assert(table->at(1).physical_handles.empty());
            if (to_pool)
            {
                allocator.deallocateToPool(*table, ring);
            }
            else
            {
                allocator.deallocate(*table);
            }
            assert(images.empty() && buffers.empty());
        }

        // Failure in a later imported record still releases the complete owned prefix.
        auto imported = description;
        imported.lifetime = ERGResourceLifetime::IMPORTED;
        ring.resources.push_back(imported);
        RGVulkanResourceAllocator allocator(resources);
        const auto invalid_import = allocator.allocate(ring, {}, {}, {8, 8}, 2);
        assert(!invalid_import && isError<err::internal::InvalidArgument>(invalid_import.error()));
        assert(images.empty() && buffers.empty());
    }

    for (const auto& description : {resource, buffer})
    {
        RGGraphDescription ordinary;
        ordinary.resources.push_back(description);
        RGVulkanResourceAllocator allocator(resources);
        auto first = allocator.allocate(ordinary, {}, {}, {8, 8}, 1);
        assert(first && images.size() + buffers.size() == 1);
        allocator.deallocateToPool(*first, ordinary);
        const auto attempts = image_attempts + buffer_attempts;
        auto reused = allocator.allocate(ordinary, {}, {}, {8, 8}, 1);
        assert(reused && image_attempts + buffer_attempts == attempts);
        allocator.deallocateToPool(*reused, ordinary);

        // A cached prefix is adopted into the candidate before the next native failure.
        reject_image_at = image_attempts + 1;
        reject_buffer_at = buffer_attempts + 1;
        auto rejected = allocator.allocate(ordinary, {}, {}, {8, 8}, 2);
        assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
        assert(images.empty() && buffers.empty());
        reject_image_at = reject_buffer_at = 0;
        auto refill = allocator.allocate(ordinary, {}, {}, {8, 8}, 1);
        assert(refill);
        allocator.deallocateToPool(*refill, ordinary);
        allocator.garbageCollect(4, 4);
        assert(images.size() + buffers.size() == 1);
        allocator.garbageCollect(5, 4);
        assert(images.empty() && buffers.empty());
    }
    assert(pools.empty());

    {
        RGVulkanResourceAllocator allocator(resources);
        auto external_owner = allocator.allocate(graph, {}, {}, {8, 8}, 1);
        assert(external_owner && images.size() == 1);
        borrowed_image = reinterpret_cast<VkImage>(external_owner->at(0).physical_handles[0]);
        RGGraphDescription borrowed;
        auto imported = resource;
        imported.lifetime = ERGResourceLifetime::IMPORTED;
        imported.import_info = std::make_unique<RGImportedResourceInfo>();
        imported.import_info->image_getter = borrowedImages;
        borrowed.resources.push_back(imported);
        auto external = resource;
        external.lifetime = ERGResourceLifetime::EXTERNAL;
        borrowed.resources.push_back(external);
        for (const bool to_pool : {false, true})
        {
            auto table = allocator.allocate(borrowed, {}, {}, {8, 8}, 2);
            assert(table && table->at(0).physical_handles.size() == 1);
            assert(table->at(1).physical_handles.empty());
            if (to_pool)
            {
                allocator.deallocateToPool(*table, borrowed);
            }
            else
            {
                allocator.deallocate(*table);
            }
            assert(images.size() == 1 && images.contains(borrowed_image));
        }
        allocator.deallocate(*external_owner);
        assert(images.empty());
    }
    assert(pools.empty());

    // Relative descriptions must retain the resolved extent as their cache key.
    {
        RGGraphDescription relative;
        auto description = resource;
        auto relative_texture = RGTextureDescription::Relative(0.5f, 0.5f);
        relative_texture.usage = texture.usage;
        description.desc = relative_texture;
        relative.resources.push_back(description);
        RGVulkanResourceAllocator allocator(resources);
        auto first = allocator.allocate(relative, {}, {}, {32, 16}, 2);
        assert(first && first->at(0).pool_key_width == 16 && first->at(0).pool_key_height == 8);
        allocator.deallocateToPool(*first, relative);
        const auto attempts = image_attempts;
        auto reused = allocator.allocate(relative, {}, {}, {32, 16}, 2);
        assert(reused && image_attempts == attempts);
        allocator.deallocateToPool(*reused, relative);
    }
    assert(images.empty() && buffers.empty() && pools.empty());
}
