#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <lux/engine/render/gpu/VulkanContext.hpp>

#include <cassert>
#include <cstdio>
#include <map>
#include <set>
#include <string_view>

namespace
{
    struct BufferOrigin
    {
        VmaAllocator allocator;
        VmaAllocation allocation;
    };

    std::map<VkBuffer, BufferOrigin> buffers;
    unsigned buffer_attempts{}, fail_buffer{};
    unsigned virtual_attempts{}, fail_virtual{};
    std::set<VmaVirtualBlock> virtual_blocks;

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
        if (++buffer_attempts == fail_buffer)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreateBuffer(allocator, info, allocation_info, buffer, allocation, result_info);
        if (result == VK_SUCCESS)
        {
            assert(buffers.emplace(*buffer, BufferOrigin{allocator, *allocation}).second);
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

    VkResult createVirtual(const VmaVirtualBlockCreateInfo* info, VmaVirtualBlock* block)
    {
        *block = nullptr;
        if (++virtual_attempts == fail_virtual)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreateVirtualBlock(info, block);
        if (result == VK_SUCCESS)
        {
            assert(virtual_blocks.insert(*block).second);
        }
        return result;
    }

    void destroyVirtual(VmaVirtualBlock block)
    {
        assert(virtual_blocks.erase(block) == 1);
        vmaDestroyVirtualBlock(block);
    }
} // namespace

// Match the shipping RelWithDebInfo production assertion policy. Test assertions
// below remain active; an ignored allocation must not be hidden by a debug abort.
// clang-format off
#define NDEBUG
#include <cassert>
#define vmaCreateBuffer createBuffer
#define vmaDestroyBuffer destroyBuffer
#define vmaCreateVirtualBlock createVirtual
#define vmaDestroyVirtualBlock destroyVirtual
#include "../src/gpu/memory/VmaTypes.cpp"
#include "../src/gpu/VulkanContext.cpp"
#include "../src/gpu/memory/ArenaAllocator.cpp"
#include "../src/gpu/memory/GPUBufferVma.cpp"
#include "../src/gpu/lifecycle/DeferredDestroyQueue.cpp"
#include "../src/resources/mesh/MeshResources.cpp"
#undef vmaDestroyVirtualBlock
#undef vmaCreateVirtualBlock
#undef vmaDestroyBuffer
#undef vmaCreateBuffer
#undef NDEBUG
#include <cassert>
// clang-format on

int main(int argc, char** argv)
{
    using namespace lux::render;
    const bool check_ssbo = argc == 2 && std::string_view(argv[1]) == "--ssbo";
    InstanceContext instance({});
    DeviceContext device(instance);
    assert(device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
    DeferredDestroyQueue retirement;
    retirement.init(device.vmaAllocator(), device.logicalDevice());
    MeshResources::InitInfo info{};
    info.device = &device;
    info.vertex_arena_bytes = 4096;
    info.index_arena_bytes = 4096;
    info.geometry_capacity_bytes = 32768;
    info.mesh_max_count = 8;
    info.segments_ssbo_cfg.initial_dense_capacity = 8;
    info.segments_ssbo_cfg.deferred_queue = &retirement;
    if (check_ssbo)
    {
        fail_buffer = 3;
        MeshResources mesh;
        const auto accepted = mesh.init(info);
        std::printf("mesh segment table failure: accepted=%d live_buffers=%zu\n", accepted, buffers.size());
        std::fflush(stdout);
        assert(!accepted);
        return 0;
    }
    for (unsigned failure = 1; failure <= 4; ++failure)
    {
        buffer_attempts = virtual_attempts = 0;
        fail_buffer = failure <= 2 ? failure : 0;
        fail_virtual = failure > 2 ? failure - 2 : 0;
        MeshResources mesh;
        assert(!mesh.init(info));
        assert(buffers.empty() && virtual_blocks.empty());
        assert(retirement.pendingCount() == 0);
    }
    fail_buffer = fail_virtual = 0;
    {
        MeshResources mesh;
        assert(mesh.init(info));
        assert(buffers.size() == 3 && virtual_blocks.size() == 2);
        assert(mesh.iboTopologySerial() == 0);
        const auto vertex = mesh.vertexBuffer();
        const auto index = mesh.indexBuffer();
        const std::vector<std::byte> data(8192);
        MeshCreateInfo mesh_info{};
        mesh_info.layout_id = 1;
        mesh_info.vertex_stride = 16;
        mesh_info.vertex_buffer = data;
        mesh_info.index_buffer = data;
        for (unsigned failure = 1; failure <= 4; ++failure)
        {
            buffer_attempts = virtual_attempts = 0;
            fail_buffer = failure <= 2 ? failure : 0;
            fail_virtual = failure > 2 ? failure - 2 : 0;
            const auto rejected = mesh.allocateOnly(mesh_info);
            assert(!rejected);
            assert(mesh.vertexBuffer() == vertex && mesh.indexBuffer() == index);
            assert(mesh.vboSegmentCount() == 1 && mesh.iboSegmentCount() == 1);
            assert(buffers.size() == 3 && virtual_blocks.size() == 2);
        }
        fail_buffer = fail_virtual = 0;
        const auto accepted = mesh.allocateOnly(mesh_info);
        assert(accepted);
        assert(mesh.vboSegmentCount() == 2 && mesh.iboSegmentCount() == 2);
        assert(mesh.destroy(accepted->handle));
    }
    // Only the published segment table is deferred; unpublished geometry cleaned synchronously.
    assert(virtual_blocks.empty() && buffers.size() == 1);
    retirement.flushAll();
    assert(buffers.empty());
    std::puts("mesh geometry ownership: initial/growth native faults, rollback, retry and cleanup PASS");
}
