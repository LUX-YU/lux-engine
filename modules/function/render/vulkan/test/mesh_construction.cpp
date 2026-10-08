#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/resources/vertex/StaticVertexPoolSet.hpp>

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

    struct DescriptorWrite
    {
        VkDescriptorSet set;
        std::uint32_t slot;
        VkBuffer buffer;
    };

    std::vector<DescriptorWrite> descriptor_writes;

    void updateDescriptors(
        VkDevice device,
        std::uint32_t count,
        const VkWriteDescriptorSet* writes,
        std::uint32_t copy_count,
        const VkCopyDescriptorSet* copies
    )
    {
        for (std::uint32_t index = 0; index < count; ++index)
        {
            assert(writes[index].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            assert(writes[index].descriptorCount == 1);
            descriptor_writes.push_back(
                {writes[index].dstSet, writes[index].dstArrayElement, writes[index].pBufferInfo->buffer}
            );
        }
        vkUpdateDescriptorSets(device, count, writes, copy_count, copies);
    }

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
#include "../src/gpu/pipeline/GeneralDescriptorSetLayout.cpp"
#include "../src/gpu/memory/ArenaAllocator.cpp"
#include "../src/gpu/memory/GPUBufferVma.cpp"
#include "../src/gpu/lifecycle/DeferredDestroyQueue.cpp"
#include "../src/resources/mesh/MeshResources.cpp"
#include "../src/resources/vertex/StaticVertexSource.cpp"
#define vkUpdateDescriptorSets updateDescriptors
#include "../src/resources/vertex/VertexPoolRegistry.cpp"
#undef vkUpdateDescriptorSets
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
    DeferredDestroyQueue first_queue(device);
    DeferredDestroyQueue second_queue(device);
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
        const auto slot_result = buffer->allocate();
        assert(slot_result);
        const auto slot = *slot_result;
        assert(buffer->isAlive(slot));
        assert(*buffer->mapped(0, slot) == 0 && *buffer->mapped(1, slot) == 0);
        buffer->write(0, slot, 17);
        buffer->write(1, slot, 29);
        const auto original = buffer->buffer();
        const auto generation = buffer->bufferGeneration();
        fail_buffer = buffer_attempts + 1;
        const auto rejected = buffer->reserve(65);
        assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
        assert(rejected.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
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
        assert(replacement && replacement->index == slot.index && replacement->gen != slot.gen);
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
        const auto added = mirror->add(41);
        assert(added);
        const auto slot = *added;
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

void checkStaticPools(lux::render::DeviceContext& device, lux::render::MeshResources& mesh)
{
    using namespace lux::render;
    auto layouts = GeneralDescriptorSetLayout::create(device);
    assert(layouts);
    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kVertexPoolMaxCount};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    auto pool = DescriptorPoolOwner::create(device.logicalDevice(), pool_info);
    assert(pool);
    const std::array set_layouts{
        (*layouts)->getLayout(EDescriptorSetSlot::VERTEX_POOL),
        (*layouts)->getLayout(EDescriptorSetSlot::VERTEX_POOL)
    };
    std::array<VkDescriptorSet, 2> targets{};
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = pool->get();
    allocation.descriptorSetCount = 2;
    allocation.pSetLayouts = set_layouts.data();
    assert(vkAllocateDescriptorSets(device.logicalDevice(), &allocation, targets.data()) == VK_SUCCESS);
    auto registry = VertexPoolRegistry::create(device, targets, 0);
    assert(registry);
    StaticVertexSource independent(mesh, 0, 1);
    assert((*registry)->registerSource(independent) == 0);
    descriptor_writes.clear();
    const auto allocation_count = buffer_attempts;
    const auto arena_count = virtual_attempts;
    {
        static_assert(!std::is_default_constructible_v<StaticVertexPoolSet>);
        static_assert(!std::is_copy_constructible_v<StaticVertexPoolSet>);
        static_assert(!std::is_move_constructible_v<StaticVertexPoolSet>);
        static_assert(std::is_nothrow_constructible_v<StaticVertexPoolSet, VertexPoolRegistry&, MeshResources&>);
        StaticVertexPoolSet pools(**registry, mesh);
        assert(descriptor_writes.empty());
        assert(buffer_attempts == allocation_count && virtual_attempts == arena_count);
        assert(pools.ensureRegistered(mesh.vboSegmentCount(), 1) == ~0u);
        assert(pools.ensureRegistered(0, kInvalidVertexLayoutId) == ~0u);
        assert(!pools.handleForMesh({}).valid());
        assert(descriptor_writes.empty());
        const auto first = pools.ensureRegistered(0, 1);
        const auto second = pools.ensureRegistered(1, 1);
        const auto other_layout = pools.ensureRegistered(0, 2);
        assert(first == 1 && second == 2 && other_layout == 3);
        assert(descriptor_writes.size() == 6);
        const std::array expected_buffers{mesh.vertexBuffer(0), mesh.vertexBuffer(1), mesh.vertexBuffer(0)};
        for (std::size_t entry = 0; entry < 3; ++entry)
        {
            for (std::size_t frame = 0; frame < targets.size(); ++frame)
            {
                const auto& write = descriptor_writes[entry * targets.size() + frame];
                assert(write.set == targets[frame] && write.slot == entry + 1);
                assert(write.buffer == expected_buffers[entry]);
            }
        }
        assert(pools.ensureRegistered(0, 1) == first && pools.ensureRegistered(1, 1) == second);
        assert(descriptor_writes.size() == 6);

        const std::vector<std::byte> bytes(64);
        MeshCreateInfo info{};
        info.layout_id = 1;
        info.vertex_stride = 16;
        info.vertex_buffer = bytes;
        info.index_buffer = bytes;
        const auto small = mesh.allocateOnly(info);
        assert(small);
        const auto* record = mesh.getGpuRecord(small->handle);
        assert(record && record->vbo_segment == 0);
        const auto handle = pools.handleForMesh(small->handle);
        assert(handle.valid() && handle.pool_id == first && handle.vertex_count == 4);
        assert(handle.vertex_base == record->vertex_buffer_range.offset / 16);
        assert(descriptor_writes.size() == 6);
        assert(mesh.destroy(small->handle));
        assert(!pools.handleForMesh(small->handle).valid());

        std::vector<std::unique_ptr<StaticVertexSource>> fillers;
        for (std::uint32_t slot = 4; slot < kVertexPoolMaxCount; ++slot)
        {
            auto source = std::make_unique<StaticVertexSource>(mesh, 0, 1);
            assert((*registry)->registerSource(*source) == slot);
            fillers.push_back(std::move(source));
        }
        const auto before_retry = descriptor_writes.size();
        assert(pools.ensureRegistered(1, 2) == ~0u);
        assert(pools.ensureRegistered(1, 2) == ~0u);
        assert(descriptor_writes.size() == before_retry);
        (*registry)->unregisterSource(4);
        assert(pools.ensureRegistered(1, 2) == 4);
        assert(descriptor_writes.size() == before_retry + targets.size());
        assert(pools.ensureRegistered(1, 2) == 4);
        assert(descriptor_writes.size() == before_retry + targets.size());
        for (std::uint32_t slot = 5; slot < kVertexPoolMaxCount; ++slot)
        {
            (*registry)->unregisterSource(slot);
        }
    }
    assert((*registry)->isRegistered(0) && independent.bindlessPoolId() == 0);
    for (std::uint32_t slot = 1; slot < kVertexPoolMaxCount; ++slot)
    {
        assert(!(*registry)->isRegistered(slot));
    }
    (*registry)->unregisterSource(0);
    assert(independent.bindlessPoolId() == ~0u);
    assert(buffer_attempts == allocation_count && virtual_attempts == arena_count);
    std::puts("Static vertex pools: native descriptors, segment/layout identity, capacity retry and revoke PASS");
}

int main(int argc, char** argv)
{
    using namespace lux::render;
    const bool check_ssbo = argc == 2 && std::string_view(argv[1]) == "--ssbo";
    auto instance_owner = InstanceContext::create({});
    assert(instance_owner);
    auto& instance = **instance_owner;
    auto device_owner = DeviceContext::create(instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
    assert(device_owner);
    auto& device = **device_owner;
    if (argc == 2 && std::string_view(argv[1]) == "--buffer")
    {
        checkBufferConstruction(device);
        return 0;
    }
    DeferredDestroyQueue retirement(device);
    MeshResources::CreateInfo info{};
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
        const auto accepted = MeshResources::create(info);
        std::printf(
            "mesh segment table failure: accepted=%d live_buffers=%zu\n",
            static_cast<bool>(accepted),
            buffers.size()
        );
        std::fflush(stdout);
        assert(!accepted && isError<err::device::VulkanCallFailed>(accepted.error()));
        assert(accepted.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        assert(buffers.empty() && virtual_blocks.empty() && retirement.pendingCount() == 0);
        return 0;
    }
    static_assert(!std::is_default_constructible_v<MeshResources>);
    static_assert(!std::is_copy_constructible_v<MeshResources>);
    static_assert(!std::is_move_constructible_v<MeshResources>);
    for (unsigned invalid = 0; invalid < 8; ++invalid)
    {
        auto bad = info;
        switch (invalid)
        {
        case 0:
            bad.device = nullptr;
            break;
        case 1:
            bad.segments_ssbo_cfg.deferred_queue = nullptr;
            break;
        case 2:
            bad.vertex_arena_bytes = 0;
            break;
        case 3:
            bad.index_arena_bytes = 0;
            break;
        case 4:
            bad.frames_in_flight = 0;
            break;
        case 5:
            bad.mesh_max_count = 0;
            break;
        case 6:
            bad.geometry_capacity_bytes = 8191;
            break;
        case 7:
            bad.vertex_arena_bytes = std::numeric_limits<VkDeviceSize>::max();
            bad.geometry_capacity_bytes = bad.vertex_arena_bytes;
            break;
        }
        const auto native_attempts = buffer_attempts;
        const auto arena_attempts = virtual_attempts;
        auto rejected = MeshResources::create(bad);
        assert(!rejected);
        if (invalid < 5)
        {
            assert(isError<err::memory::InvalidMeshConfiguration>(rejected.error()));
        }
        else
        {
            assert(isError<err::memory::CapacityExhausted>(rejected.error()));
        }
        assert(buffer_attempts == native_attempts && virtual_attempts == arena_attempts);
        assert(buffers.empty() && virtual_blocks.empty() && retirement.pendingCount() == 0);
    }
    for (unsigned failure = 1; failure <= 4; ++failure)
    {
        buffer_attempts = virtual_attempts = 0;
        fail_buffer = failure <= 2 ? failure : 0;
        fail_virtual = failure > 2 ? failure - 2 : 0;
        ResourceRegistry registry;
        const auto rejected = MeshResources::create(info);
        assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
        assert(rejected.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        assert(registry.size() == 0 && !registry.find<MeshResources>());
        assert(buffers.empty() && virtual_blocks.empty());
        assert(retirement.pendingCount() == 0);
    }
    fail_buffer = fail_virtual = 0;
    {
        ResourceRegistry registry;
        auto candidate = MeshResources::create(info);
        assert(candidate && registry.size() == 0 && !registry.find<MeshResources>());
        auto* address = candidate->get();
        const auto handle = registry.insert(std::move(*candidate));
        assert(!*candidate && handle.get() == address);
        assert(registry.size() == 1 && registry.find<MeshResources>() == address);
        auto& mesh = *handle;
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
            assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
            assert(rejected.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
            assert(mesh.vertexBuffer() == vertex && mesh.indexBuffer() == index);
            assert(mesh.vboSegmentCount() == 1 && mesh.iboSegmentCount() == 1);
            assert(mesh.iboTopologySerial() == 0 && !mesh.lastCapacityShortfall());
            assert(buffers.size() == 3 && virtual_blocks.size() == 2);
        }
        fail_buffer = fail_virtual = 0;
        const auto accepted = mesh.allocateOnly(mesh_info);
        assert(accepted);
        assert(mesh.vboSegmentCount() == 2 && mesh.iboSegmentCount() == 2);
        assert(mesh.iboTopologySerial() == 1 && mesh.alive(accepted->handle));
        checkStaticPools(device, mesh);
        assert(mesh.destroy(accepted->handle));
        assert(!mesh.alive(accepted->handle));
        mesh.retireFrameStagingBuffers(0);
        assert(mesh.vboTelemetry().used_bytes == 0 && mesh.iboTelemetry().used_bytes == 0);
    }
    // Only the published segment table is deferred; unpublished geometry cleaned synchronously.
    assert(virtual_blocks.empty() && buffers.size() == 1);
    retirement.flushAll();
    assert(buffers.empty());
    std::puts("mesh geometry ownership: initial/growth native faults, rollback, retry and cleanup PASS");
}
