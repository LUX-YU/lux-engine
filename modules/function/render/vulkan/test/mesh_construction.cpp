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

void checkBufferConstruction(lux::render::DeviceContext& device)
{
    using namespace lux::render;
    using Buffer = DynamicSSBO<std::uint32_t>;
    static_assert(!std::is_default_constructible_v<Buffer>);
    static_assert(!std::is_constructible_v<Buffer, GpuBufferCreateInfo>);
    static_assert(!std::is_copy_constructible_v<Buffer>);
    static_assert(std::is_nothrow_move_constructible_v<Buffer>);
    DeferredDestroyQueue first_queue;
    DeferredDestroyQueue second_queue;
    first_queue.init(device.vmaAllocator(), device.logicalDevice());
    second_queue.init(device.vmaAllocator(), device.logicalDevice());
    first_queue.beginFrame(7);
    second_queue.beginFrame(11);
    GpuBufferCreateInfo
        info{.device_context = &device, .deferred_queue = &first_queue, .initial_capacity = 3, .slices = 2};
    for (unsigned invalid = 0; invalid < 5; ++invalid)
    {
        auto bad = info;
        switch (invalid)
        {
        case 0:
            bad.device_context = nullptr;
            break;
        case 1:
            bad.deferred_queue = nullptr;
            break;
        case 2:
            bad.initial_capacity = 0;
            break;
        case 3:
            bad.slices = 0;
            break;
        case 4:
            bad.initial_capacity = std::numeric_limits<std::uint32_t>::max();
            break;
        }
        const auto attempts = buffer_attempts;
        auto rejected = Buffer::create(bad);
        assert(!rejected && isError<err::memory::InvalidBufferConfiguration>(rejected.error()));
        assert(buffer_attempts == attempts && buffers.empty());
    }
    fail_buffer = buffer_attempts + 1;
    auto failed = Buffer::create(info);
    assert(!failed && isError<err::device::VulkanCallFailed>(failed.error()));
    assert(failed.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
    assert(buffers.empty() && first_queue.pendingCount() == 0);
    fail_buffer = 0;
    {
        auto buffer = Buffer::create(info);
        assert(buffer && buffer->capacity() == 64 && buffer->slices() == 2);
        const auto slot = buffer->allocate();
        assert(buffer->isAlive(slot));
        assert(*buffer->mapped(0, slot) == 0 && *buffer->mapped(1, slot) == 0);
        buffer->write(0, slot, 17);
        buffer->write(1, slot, 29);
        const auto original = buffer->buffer();
        const auto generation = buffer->bufferGeneration();
        fail_buffer = buffer_attempts + 1;
        assert(!buffer->reserve(65));
        assert(buffer->buffer() == original && buffer->bufferGeneration() == generation);
        assert(buffer->capacity() == 64 && buffer->isAlive(slot));
        assert(*buffer->mapped(0, slot) == 17 && *buffer->mapped(1, slot) == 29);
        assert(first_queue.pendingCount() == 0 && buffers.size() == 1);
        fail_buffer = 0;
        assert(buffer->reserve(65));
        assert(buffer->capacity() == 128 && buffer->bufferGeneration() == generation + 1);
        assert(buffer->isAlive(slot));
        assert(*buffer->mapped(0, slot) == 17 && *buffer->mapped(1, slot) == 29);
        assert(first_queue.pendingCount() == 1 && buffers.size() == 2);
        info.deferred_queue = &second_queue;
        auto destination = Buffer::create(info);
        assert(destination && buffers.size() == 3);
        *destination = std::move(*buffer);
        assert(second_queue.pendingCount() == 1 && first_queue.pendingCount() == 1);
        assert(destination->isAlive(slot) && *destination->mapped(1, slot) == 29);
        assert(!buffer->buffer());
        Buffer moved(std::move(*destination));
        assert(!destination->buffer() && moved.isAlive(slot));
        assert(moved.free(slot) && !moved.isAlive(slot));
        const auto replacement = moved.allocate();
        assert(replacement.index == slot.index && replacement.gen != slot.gen);
    }
    assert(first_queue.pendingCount() == 2 && second_queue.pendingCount() == 1);
    first_queue.collect(6);
    second_queue.collect(10);
    assert(buffers.size() == 3);
    first_queue.collect(7);
    assert(buffers.size() == 1 && first_queue.pendingCount() == 0);
    second_queue.collect(11);
    assert(buffers.empty() && second_queue.pendingCount() == 0);
    info.deferred_queue = &first_queue;
    info.clear_on_remove = true;
    {
        auto mirror = SlicedSSBO<std::uint32_t>::create(info);
        assert(mirror);
        const auto slot = mirror->add(41);
        const auto epoch = mirror->globalEpoch();
        assert(mirror->hostValue(slot.index) == 41);
        assert(mirror->uploadDataSliceDeferred(0));
        assert(*mirror->mapped(0, slot) == 41);
        assert(mirror->modify(slot, 53) && mirror->globalEpoch() > epoch);
        assert(mirror->reserve(65));
        assert(mirror->hostValue(slot.index) == 53);
        assert(*mirror->mapped(0, slot) == 53 && *mirror->mapped(1, slot) == 53);
        assert(mirror->remove(slot));
        assert(mirror->uploadDataSliceDeferred(1));
        assert(mirror->hostValue(slot.index) == 0);
        auto gpu = StaticBuffer<std::uint32_t>::create(info);
        assert(gpu && gpu->slices() == 1 && gpu->capacity() == 64);
        const auto original = gpu->buffer();
        fail_buffer = buffer_attempts + 1;
        assert(!gpu->reserve(65, false) && gpu->buffer() == original);
        fail_buffer = 0;
        assert(gpu->reserve(65, false) && gpu->capacity() == 128);
    }
    assert(first_queue.pendingCount() == 4 && buffers.size() == 4);
    first_queue.collect(7);
    assert(buffers.empty());
    std::puts("GPU buffer construction: native failure, growth, data, slot identity, move and retirement PASS");
}

int main(int argc, char** argv)
{
    using namespace lux::render;
    const bool check_ssbo = argc == 2 && std::string_view(argv[1]) == "--ssbo";
    InstanceContext instance({});
    DeviceContext device(instance);
    assert(device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
    if (argc == 2 && std::string_view(argv[1]) == "--buffer")
    {
        checkBufferConstruction(device);
        return 0;
    }
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
        assert(buffers.empty() && virtual_blocks.empty() && retirement.pendingCount() == 0);
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
